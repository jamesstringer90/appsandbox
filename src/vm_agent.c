#include <winsock2.h>
#include "vm_agent.h"
#include "ui.h"
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

static const GUID AGENT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0001, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Agent status notification ---- */

#define WM_VM_AGENT_STATUS      (WM_APP + 2)
#define WM_VM_AGENT_SHUTDOWN    (WM_APP + 3)
#define WM_VM_AGENT_GPUCOPY     (WM_APP + 4)
#define WM_VM_HYPERV_VIDEO_OFF  (WM_APP + 12)

static HWND g_agent_hwnd = NULL;

void vm_agent_set_hwnd(HWND hwnd)
{
    g_agent_hwnd = hwnd;
}

/* ---- Per-VM connection state ---- */

typedef struct AgentConn {
    VmInstance    *vm;
    HANDLE         thread;
    SOCKET         sock;
    volatile BOOL  stop;
    /* Command synchronization */
    volatile BOOL  cmd_pending;
    HANDLE         cmd_done;     /* Event: signaled when response is ready */
    char           cmd[64];
    char           rsp[256];
} AgentConn;

#define MAX_AGENTS 16
static AgentConn g_conns[MAX_AGENTS];
static BOOL      g_wsa_init = FALSE;

static AgentConn *find_conn(VmInstance *vm)
{
    int i;
    for (i = 0; i < MAX_AGENTS; i++)
        if (g_conns[i].vm == vm)
            return &g_conns[i];
    return NULL;
}

static AgentConn *alloc_conn(VmInstance *vm)
{
    int i;
    for (i = 0; i < MAX_AGENTS; i++) {
        if (g_conns[i].vm == NULL) {
            memset(&g_conns[i], 0, sizeof(AgentConn));
            g_conns[i].vm = vm;
            g_conns[i].sock = INVALID_SOCKET;
            g_conns[i].cmd_done = CreateEventW(NULL, FALSE, FALSE, NULL);
            return &g_conns[i];
        }
    }
    return NULL;
}

static void free_conn(AgentConn *conn)
{
    if (conn->cmd_done) CloseHandle(conn->cmd_done);
    if (conn->thread) CloseHandle(conn->thread);
    memset(conn, 0, sizeof(AgentConn));
    conn->sock = INVALID_SOCKET;
}

/* ---- Line I/O ---- */

/* Read a single line (up to \n) from socket. Returns length, 0 on close, -1 on error. */
static int recv_line(SOCKET s, char *buf, int buf_size)
{
    int pos = 0;
    while (pos < buf_size - 1) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int send_line(SOCKET s, const char *msg)
{
    int len = (int)strlen(msg);
    int n;
    n = send(s, msg, len, 0);
    if (n <= 0) return n;
    n = send(s, "\n", 1, 0);
    return n;
}

/* ---- RuntimeId lookup ---- */

static BOOL get_vm_runtime_id(VmInstance *instance, GUID *out)
{
    static const GUID zero_guid = {0};

    if (memcmp(&instance->runtime_id, &zero_guid, sizeof(GUID)) != 0) {
        *out = instance->runtime_id;
        return TRUE;
    }

    if (hcs_find_runtime_id(instance->name, out)) {
        instance->runtime_id = *out;
        return TRUE;
    }

    return FALSE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_agent(VmInstance *vm, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    GUID runtime_id;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;

    if (!get_vm_runtime_id(vm, &runtime_id))
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = runtime_id;
    addr.ServiceId = AGENT_SERVICE_GUID;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with timeouts */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ---- Notify UI of agent status change ---- */

static void notify_agent_status(VmInstance *vm)
{
    if (g_agent_hwnd)
        PostMessageW(g_agent_hwnd, WM_VM_AGENT_STATUS, 0, (LPARAM)vm);
}

/* ---- Persistent connection thread ---- */

static DWORD WINAPI agent_thread_proc(LPVOID param)
{
    AgentConn *conn = (AgentConn *)param;
    VmInstance *vm = conn->vm;

    while (!conn->stop) {
        char buf[256];
        int n;
        SOCKET s;

        /* Try to connect */
        s = connect_to_agent(vm, 3000);
        if (s == INVALID_SOCKET) {
            /* Retry in 3 seconds, checking stop flag each second */
            int wait;
            for (wait = 0; wait < 3000 && !conn->stop; wait += 500)
                Sleep(500);
            continue;
        }

        conn->sock = s;

        /* Wait for hello from agent */
        n = recv_line(s, buf, sizeof(buf));
        if (n <= 0 || strcmp(buf, "hello") != 0) {
            closesocket(s);
            conn->sock = INVALID_SOCKET;
            continue;
        }

        vm->agent_online = TRUE;
        vm->shutdown_requested = FALSE;
        vm->last_heartbeat = GetTickCount64();
        ui_log(L"Agent online for \"%s\".", vm->name);

        /* Mark install complete on first agent connection */
        if (!vm->install_complete && !vm->is_template) {
            vm->install_complete = TRUE;
            vm_save_state_json(vm->vhdx_path, TRUE);
            ui_log(L"Install complete for \"%s\".", vm->name);
        }

        notify_agent_status(vm);

        /* Send NAT IP to agent (only for NAT mode) */
        if (vm->network_mode == NET_NAT && vm->nat_ip[0] != '\0') {
            char ip_cmd[64];
            sprintf_s(ip_cmd, sizeof(ip_cmd), "set_ip:%s/16:172.20.0.1", vm->nat_ip);
            send_line(s, ip_cmd);
            n = recv_line(s, buf, sizeof(buf));
            if (n > 0)
                ui_log(L"NAT IP config for \"%s\": %S", vm->name, buf);
        }

        /* Send GPU share info to agent (if GPU-PV is assigned) */
        if (vm->gpu_mode != 0 && vm->gpu_shares.count > 0) {
            char header[64];
            int gi;
            sprintf_s(header, sizeof(header), "gpu_query_response:%d",
                      vm->gpu_shares.count);
            send_line(s, header);
            for (gi = 0; gi < vm->gpu_shares.count; gi++) {
                const GpuDriverShare *ds = &vm->gpu_shares.shares[gi];
                char line[8192];
                char share_a[128], dest_a[512], filter_a[4096];

                WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                    share_a, sizeof(share_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->guest_path, -1,
                                    dest_a, sizeof(dest_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->file_filter, -1,
                                    filter_a, sizeof(filter_a), NULL, NULL);

                sprintf_s(line, sizeof(line), "%s|%s|%s",
                          share_a, dest_a, filter_a);
                send_line(s, line);
            }
            ui_log(L"Sent %d GPU share(s) to agent for \"%s\".",
                   vm->gpu_shares.count, vm->name);
        } else {
            send_line(s, "gpu_none");
        }

        /* Connected — read loop */
        while (!conn->stop) {
            fd_set rfds;
            struct timeval tv;
            int ret;

            /* Check for pending command first */
            if (conn->cmd_pending) {
                if (send_line(s, conn->cmd) <= 0) break;
                n = recv_line(s, conn->rsp, sizeof(conn->rsp));
                if (n <= 0) {
                    conn->rsp[0] = '\0';
                    conn->cmd_pending = FALSE;
                    SetEvent(conn->cmd_done);
                    break;
                }
                conn->cmd_pending = FALSE;
                SetEvent(conn->cmd_done);
                continue;
            }

            /* Wait for data with 200ms timeout */
            FD_ZERO(&rfds);
            FD_SET(s, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 200000;

            ret = select(0, &rfds, NULL, NULL, &tv);
            if (ret < 0) break;
            if (ret == 0) continue; /* timeout — loop back to check cmd_pending/stop */

            n = recv_line(s, buf, sizeof(buf));
            if (n <= 0) break; /* connection lost */

            /* Process messages from agent */
            if (strcmp(buf, "heartbeat") == 0) {
                vm->last_heartbeat = GetTickCount64();
            } else if (strcmp(buf, "os_shutdown") == 0) {
                ui_log(L"Guest OS shutting down for \"%s\".", vm->name);
                vm->agent_online = FALSE;
                vm->shutdown_requested = TRUE;
                vm->shutdown_time = GetTickCount64();
                notify_agent_status(vm);
                /* Don't set conn->stop — let outer loop retry in case of reboot.
                   HCS SystemExited will call vm_agent_stop() on real shutdown. */
                break;
            } else if (strcmp(buf, "service_stopping") == 0) {
                ui_log(L"Agent service stopped in \"%s\".", vm->name);
                conn->stop = TRUE;
                break;
            } else if (strncmp(buf, "gpu_copy_progress:", 18) == 0) {
                ui_log(L"GPU copy progress for \"%s\": %S", vm->name, buf + 18);
            } else if (strncmp(buf, "gpu_copy_done:", 14) == 0) {
                ui_log(L"GPU copy complete for \"%s\" (%S files).",
                       vm->name, buf + 14);
            } else if (strncmp(buf, "gpu_copy_error:", 15) == 0) {
                ui_log(L"GPU copy error for \"%s\": %S",
                       vm->name, buf + 15);
            } else if (strncmp(buf, "gpu_device_status:", 18) == 0) {
                ui_log(L"[%s] GPU: %S", vm->name, buf + 18);
            } else if (strcmp(buf, "gpu_device_ok") == 0) {
                ui_log(L"[%s] GPU device recovered successfully.", vm->name);
            } else if (strncmp(buf, "gpu_device_failed:", 18) == 0) {
                ui_log(L"[%s] GPU device still failing (problem %S).",
                       vm->name, buf + 18);
            } else if (strncmp(buf, "idd_status:", 11) == 0) {
                ui_log(L"[%s] IDD driver: %S", vm->name, buf + 11);
            } else if (strncmp(buf, "hyperv_video:", 13) == 0) {
                ui_log(L"[%s] Hyper-V Video: %S", vm->name, buf + 13);
                if (strcmp(buf + 13, "disabled") == 0)
                    PostMessageW(g_agent_hwnd, WM_VM_HYPERV_VIDEO_OFF, 0, (LPARAM)vm);
            } else if (strncmp(buf, "displays:", 9) == 0) {
                ui_log(L"[%s] Displays: %S", vm->name, buf + 9);
            } else if (strncmp(buf, "log:", 4) == 0) {
                ui_log(L"[%s] %S", vm->name, buf + 4);
            } else if (strcmp(buf, "gpu_query") == 0) {
                /* Agent is asking for GPU shares (re-trigger) */
                if (vm->gpu_mode != 0 && vm->gpu_shares.count > 0) {
                    char header[64];
                    int gi;
                    sprintf_s(header, sizeof(header), "gpu_query_response:%d",
                              vm->gpu_shares.count);
                    send_line(s, header);
                    for (gi = 0; gi < vm->gpu_shares.count; gi++) {
                        const GpuDriverShare *ds = &vm->gpu_shares.shares[gi];
                        char line[8192];
                        char share_a[128], dest_a[512], filter_a[4096];

                        WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                            share_a, sizeof(share_a), NULL, NULL);
                        WideCharToMultiByte(CP_UTF8, 0, ds->guest_path, -1,
                                            dest_a, sizeof(dest_a), NULL, NULL);
                        WideCharToMultiByte(CP_UTF8, 0, ds->file_filter, -1,
                                            filter_a, sizeof(filter_a), NULL, NULL);

                        sprintf_s(line, sizeof(line), "%s|%s|%s",
                                  share_a, dest_a, filter_a);
                        send_line(s, line);
                    }
                } else {
                    send_line(s, "gpu_none");
                }
            }
        }

        /* Connection lost */
        vm->agent_online = FALSE;
        closesocket(s);
        conn->sock = INVALID_SOCKET;
        ui_log(L"Agent offline for \"%s\".", vm->name);
        notify_agent_status(vm);

        /* Wake up any blocked command sender */
        if (conn->cmd_pending) {
            conn->rsp[0] = '\0';
            conn->cmd_pending = FALSE;
            SetEvent(conn->cmd_done);
        }

        /* Don't reconnect if the VM is no longer running */
        if (!vm->running)
            break;
    }

    return 0;
}

/* ---- Public API ---- */

void vm_agent_start(VmInstance *instance)
{
    AgentConn *conn;
    WSADATA wsa;

    if (!g_wsa_init) {
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_wsa_init = TRUE;
    }

    /* Already running? */
    conn = find_conn(instance);
    if (conn && conn->thread) return;

    conn = alloc_conn(instance);
    if (!conn) {
        ui_log(L"Agent: too many connections");
        return;
    }

    conn->stop = FALSE;
    conn->thread = CreateThread(NULL, 0, agent_thread_proc, conn, 0, NULL);
}

void vm_agent_stop(VmInstance *instance)
{
    AgentConn *conn = find_conn(instance);
    if (!conn) return;

    conn->stop = TRUE;

    /* Unblock recv/select by closing the socket */
    if (conn->sock != INVALID_SOCKET) {
        closesocket(conn->sock);
        conn->sock = INVALID_SOCKET;
    }

    if (conn->thread) {
        WaitForSingleObject(conn->thread, 5000);
    }

    instance->agent_online = FALSE;
    free_conn(conn);
    notify_agent_status(instance);
}

BOOL vm_agent_send(VmInstance *instance, const char *command,
                   char *response, int response_max)
{
    AgentConn *conn = find_conn(instance);
    BOOL ok;

    if (!conn || !instance->agent_online) {
        ui_log(L"Agent: not connected to \"%s\"", instance->name);
        return FALSE;
    }

    /* Queue command for the connection thread */
    ResetEvent(conn->cmd_done);
    strcpy_s(conn->cmd, sizeof(conn->cmd), command);
    conn->cmd_pending = TRUE;

    /* Wait for response (up to 5 seconds) */
    if (WaitForSingleObject(conn->cmd_done, 5000) != WAIT_OBJECT_0) {
        ui_log(L"Agent: command \"%S\" timed out", command);
        conn->cmd_pending = FALSE;
        return FALSE;
    }

    if (response && response_max > 0)
        strncpy_s(response, response_max, conn->rsp, _TRUNCATE);

    ok = (strcmp(conn->rsp, "ok") == 0);
    ui_log(L"Agent: %S -> %S", command, conn->rsp);
    return ok;
}

BOOL vm_agent_shutdown(VmInstance *instance)
{
    return vm_agent_send(instance, "shutdown", NULL, 0);
}

BOOL vm_agent_restart(VmInstance *instance)
{
    return vm_agent_send(instance, "restart", NULL, 0);
}

BOOL vm_agent_ping(VmInstance *instance)
{
    return vm_agent_send(instance, "ping", NULL, 0);
}
