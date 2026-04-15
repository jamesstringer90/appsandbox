/*
 * vm_ssh_proxy.c — Host-side TCP-to-HV-socket proxy for SSH.
 *
 * For each VM with ssh_enabled, binds 127.0.0.1:<ephemeral> and relays
 * TCP connections to the guest agent's SSH proxy thread over AF_HYPERV
 * service GUID :0007.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include "vm_ssh_proxy.h"
#include "asb_core.h"
#include "ui.h"
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

static const GUID SSH_SERVICE_GUID =
    { 0xa5b0cafe, 0x0007, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Per-VM proxy state ---- */

#define SSH_RELAY_BUF  8192
#define MAX_SSH_RELAYS 16

typedef struct SshRelay {
    SOCKET   tcp_sock;
    SOCKET   hv_sock;
    HANDLE   thread;
    volatile BOOL stop;
} SshRelay;

typedef struct SshProxy {
    VmInstance    *vm;
    SOCKET         listen_sock;
    HANDLE         thread;
    volatile BOOL  stop;
    SshRelay       relays[MAX_SSH_RELAYS];
    CRITICAL_SECTION cs;
} SshProxy;

/* We store a pointer in a simple static array keyed by VmInstance pointer.
   At most ASB_MAX_VMS (32) proxies can exist. */
#define MAX_PROXIES 32
static SshProxy *g_proxies[MAX_PROXIES];
static CRITICAL_SECTION g_proxy_cs;
static volatile BOOL g_proxy_cs_init = FALSE;

static void ensure_cs_init(void)
{
    if (!g_proxy_cs_init) {
        InitializeCriticalSection(&g_proxy_cs);
        g_proxy_cs_init = TRUE;
    }
}

static SshProxy *find_proxy(VmInstance *vm)
{
    int i;
    for (i = 0; i < MAX_PROXIES; i++)
        if (g_proxies[i] && g_proxies[i]->vm == vm)
            return g_proxies[i];
    return NULL;
}

/* ---- HV socket connect (non-blocking with timeout) ---- */

static SOCKET connect_to_hv_ssh(const GUID *vm_runtime_id)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family    = AF_HYPERV;
    addr.VmId      = *vm_runtime_id;
    addr.ServiceId = SSH_SERVICE_GUID;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec  = 3;
        tv.tv_usec = 0;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ---- Relay thread (bidirectional copy) ---- */

static DWORD WINAPI relay_thread(LPVOID param)
{
    SshRelay *r = (SshRelay *)param;
    char buf[SSH_RELAY_BUF];
    fd_set rfds;
    struct timeval tv;
    int n;
    SOCKET max_fd;

    while (!r->stop) {
        FD_ZERO(&rfds);
        FD_SET(r->tcp_sock, &rfds);
        FD_SET(r->hv_sock, &rfds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        max_fd = (r->tcp_sock > r->hv_sock) ? r->tcp_sock : r->hv_sock;
        n = select((int)max_fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) break;
        if (n == 0) continue;

        if (FD_ISSET(r->tcp_sock, &rfds)) {
            n = recv(r->tcp_sock, buf, SSH_RELAY_BUF, 0);
            if (n <= 0) break;
            if (send(r->hv_sock, buf, n, 0) != n) break;
        }

        if (FD_ISSET(r->hv_sock, &rfds)) {
            n = recv(r->hv_sock, buf, SSH_RELAY_BUF, 0);
            if (n <= 0) break;
            if (send(r->tcp_sock, buf, n, 0) != n) break;
        }
    }

    closesocket(r->tcp_sock);
    closesocket(r->hv_sock);
    r->tcp_sock = INVALID_SOCKET;
    r->hv_sock  = INVALID_SOCKET;
    return 0;
}

/* ---- Listener thread ---- */

static DWORD WINAPI ssh_listener_thread(LPVOID param)
{
    SshProxy *proxy = (SshProxy *)param;
    struct sockaddr_in bind_addr;
    int addr_len;
    SOCKET client;
    fd_set rfds;
    struct timeval tv;
    int i;

    /* Bind ephemeral port on localhost */
    proxy->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxy->listen_sock == INVALID_SOCKET) {
        ui_log(L"SSH proxy: socket() failed for \"%s\".", proxy->vm->name);
        return 1;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;  /* OS picks ephemeral port */

    /* If we have a previously persisted port, try that first */
    if (proxy->vm->ssh_port != 0) {
        bind_addr.sin_port = htons((u_short)proxy->vm->ssh_port);
        if (bind(proxy->listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0)
            goto bound;
        /* Port in use — fall back to ephemeral */
        bind_addr.sin_port = 0;
    }

    if (bind(proxy->listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ui_log(L"SSH proxy: bind() failed for \"%s\" (%d).",
               proxy->vm->name, WSAGetLastError());
        closesocket(proxy->listen_sock);
        proxy->listen_sock = INVALID_SOCKET;
        return 1;
    }

bound:
    /* Read back the actual port */
    addr_len = sizeof(bind_addr);
    getsockname(proxy->listen_sock, (struct sockaddr *)&bind_addr, &addr_len);
    proxy->vm->ssh_port = ntohs(bind_addr.sin_port);

    if (listen(proxy->listen_sock, 4) != 0) {
        ui_log(L"SSH proxy: listen() failed for \"%s\".", proxy->vm->name);
        closesocket(proxy->listen_sock);
        proxy->listen_sock = INVALID_SOCKET;
        return 1;
    }

    ui_log(L"SSH proxy listening on 127.0.0.1:%lu for \"%s\".",
           proxy->vm->ssh_port, proxy->vm->name);

    /* Persist the assigned port */
    asb_save();

    /* Accept loop */
    while (!proxy->stop) {
        FD_ZERO(&rfds);
        FD_SET(proxy->listen_sock, &rfds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        if (select(0, &rfds, NULL, NULL, &tv) <= 0)
            continue;

        client = accept(proxy->listen_sock, NULL, NULL);
        if (client == INVALID_SOCKET)
            continue;

        /* Connect to guest SSH proxy via HV socket */
        SOCKET hv = connect_to_hv_ssh(&proxy->vm->runtime_id);
        if (hv == INVALID_SOCKET) {
            ui_log(L"SSH proxy: cannot connect to guest for \"%s\".", proxy->vm->name);
            closesocket(client);
            continue;
        }

        /* Find a free relay slot */
        EnterCriticalSection(&proxy->cs);
        for (i = 0; i < MAX_SSH_RELAYS; i++) {
            if (proxy->relays[i].tcp_sock == INVALID_SOCKET &&
                proxy->relays[i].hv_sock == INVALID_SOCKET &&
                proxy->relays[i].thread == NULL)
                break;
            /* Reap finished relay threads */
            if (proxy->relays[i].thread != NULL &&
                proxy->relays[i].tcp_sock == INVALID_SOCKET) {
                WaitForSingleObject(proxy->relays[i].thread, 0);
                CloseHandle(proxy->relays[i].thread);
                proxy->relays[i].thread = NULL;
            }
            if (proxy->relays[i].tcp_sock == INVALID_SOCKET &&
                proxy->relays[i].hv_sock == INVALID_SOCKET &&
                proxy->relays[i].thread == NULL)
                break;
        }

        if (i < MAX_SSH_RELAYS) {
            proxy->relays[i].tcp_sock = client;
            proxy->relays[i].hv_sock  = hv;
            proxy->relays[i].stop     = FALSE;
            proxy->relays[i].thread   = CreateThread(NULL, 0, relay_thread,
                                                      &proxy->relays[i], 0, NULL);
        } else {
            ui_log(L"SSH proxy: max connections reached for \"%s\".", proxy->vm->name);
            closesocket(client);
            closesocket(hv);
        }
        LeaveCriticalSection(&proxy->cs);
    }

    /* Clean up relay threads */
    EnterCriticalSection(&proxy->cs);
    for (i = 0; i < MAX_SSH_RELAYS; i++) {
        if (proxy->relays[i].thread) {
            proxy->relays[i].stop = TRUE;
            if (proxy->relays[i].tcp_sock != INVALID_SOCKET)
                closesocket(proxy->relays[i].tcp_sock);
            if (proxy->relays[i].hv_sock != INVALID_SOCKET)
                closesocket(proxy->relays[i].hv_sock);
            WaitForSingleObject(proxy->relays[i].thread, 3000);
            CloseHandle(proxy->relays[i].thread);
            proxy->relays[i].thread   = NULL;
            proxy->relays[i].tcp_sock = INVALID_SOCKET;
            proxy->relays[i].hv_sock  = INVALID_SOCKET;
        }
    }
    LeaveCriticalSection(&proxy->cs);

    closesocket(proxy->listen_sock);
    proxy->listen_sock = INVALID_SOCKET;
    return 0;
}

/* ---- Public API ---- */

void vm_ssh_proxy_start(VmInstance *instance)
{
    SshProxy *proxy;
    int i, slot;

    if (!instance || !instance->ssh_enabled)
        return;

    ensure_cs_init();
    EnterCriticalSection(&g_proxy_cs);

    /* Already running? */
    if (find_proxy(instance)) {
        LeaveCriticalSection(&g_proxy_cs);
        return;
    }

    /* Find free slot */
    slot = -1;
    for (i = 0; i < MAX_PROXIES; i++) {
        if (!g_proxies[i]) { slot = i; break; }
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_proxy_cs);
        ui_log(L"SSH proxy: no free slots.");
        return;
    }

    proxy = (SshProxy *)calloc(1, sizeof(SshProxy));
    if (!proxy) {
        LeaveCriticalSection(&g_proxy_cs);
        return;
    }

    proxy->vm = instance;
    proxy->listen_sock = INVALID_SOCKET;
    proxy->stop = FALSE;
    InitializeCriticalSection(&proxy->cs);

    for (i = 0; i < MAX_SSH_RELAYS; i++) {
        proxy->relays[i].tcp_sock = INVALID_SOCKET;
        proxy->relays[i].hv_sock  = INVALID_SOCKET;
        proxy->relays[i].thread   = NULL;
    }

    proxy->thread = CreateThread(NULL, 0, ssh_listener_thread, proxy, 0, NULL);
    if (!proxy->thread) {
        DeleteCriticalSection(&proxy->cs);
        free(proxy);
        LeaveCriticalSection(&g_proxy_cs);
        return;
    }

    g_proxies[slot] = proxy;
    LeaveCriticalSection(&g_proxy_cs);
}

void vm_ssh_proxy_stop(VmInstance *instance)
{
    SshProxy *proxy;
    int i;

    if (!instance) return;

    ensure_cs_init();
    EnterCriticalSection(&g_proxy_cs);

    proxy = find_proxy(instance);
    if (!proxy) {
        LeaveCriticalSection(&g_proxy_cs);
        return;
    }

    /* Remove from array first */
    for (i = 0; i < MAX_PROXIES; i++) {
        if (g_proxies[i] == proxy) { g_proxies[i] = NULL; break; }
    }
    LeaveCriticalSection(&g_proxy_cs);

    /* Signal stop and close listen socket to unblock accept */
    proxy->stop = TRUE;
    if (proxy->listen_sock != INVALID_SOCKET)
        closesocket(proxy->listen_sock);

    if (proxy->thread) {
        WaitForSingleObject(proxy->thread, 5000);
        CloseHandle(proxy->thread);
    }

    DeleteCriticalSection(&proxy->cs);
    free(proxy);

    ui_log(L"SSH proxy stopped for \"%s\".", instance->name);
}
