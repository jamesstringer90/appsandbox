/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-006 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-007 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-008 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-009 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-014 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-011::REQ-025 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-011::REQ-027 */
/*
 * appsandbox-gamepad.c -- Guest-side virtual Xbox 360 gamepad helper.
 *
 * Runs as a background helper (SYSTEM) supervised by appsandbox-agent.exe.
 * Listens on asb_transport channel ASB_CH_GAMEPAD (channel 8), emits
 * GAMEPAD_READY_MAGIC ("GRDY") upon accept, plugs/unplugs first-party UMDF2
 * virtual Xbox 360 controllers (AppSandboxVGD_XUSB + AppSandboxVGD_HID) for
 * slots 0..3 via per-slot shared memory + dynamic SetupDi Root devnodes, and
 * forwards async dual-motor rumble notifications back to the host.
 *
 * Logs to C:\Windows\AppSandbox\gamepad.log.
 */

#include "../transport/asb_transport.h"
#include "vgd_controller.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

static void gamepad_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\gamepad.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static int gp_send_all(AsbConn *c, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int left = len;
    while (left > 0) {
        int n = asb_send(c, p, left);
        if (n <= 0) return -1;
        p += n;
        left -= n;
    }
    return 0;
}

static int gp_recv_all(AsbConn *c, void *buf, int len)
{
    uint8_t *p = (uint8_t *)buf;
    int left = len;
    while (left > 0) {
        int n = asb_recv(c, p, left);
        if (n <= 0) return -1;
        p += n;
        left -= n;
    }
    return 0;
}

typedef struct RumbleWorker {
    HANDLE            thread;
    HANDLE            stop_event;
    VgdWinBackend    *backend;
    uint8_t           slot;
    AsbConn          *conn;
    CRITICAL_SECTION *send_cs;
    volatile LONG    *rumble_seq;
} RumbleWorker;

typedef struct GuestGamepadCtx {
    VgdWinBackend    *backend;
    VgdControllerMgr *mgr;
    VgdBackendOps    *ops;
    AsbConn          *conn;
    CRITICAL_SECTION  send_cs;
    CRITICAL_SECTION  mgr_cs;
    HANDLE            flush_stop_evt;
    HANDLE            flush_thread;
    volatile LONG     rumble_seq;
    RumbleWorker      workers[GAMEPAD_MAX_SLOTS];
} GuestGamepadCtx;

/* Flushes deferred short-tap button releases on a 2 ms tick (REQ-025) and syncs
 * newly mounted interactive user hives' joy.cpl OEMName entries every 2 s (REQ-027). */
static DWORD WINAPI flush_thread_proc(LPVOID param)
{
    GuestGamepadCtx *g = (GuestGamepadCtx *)param;
    uint64_t last_oem_sync_ms = 0;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        uint64_t now_ms;
        if (WaitForSingleObject(g->flush_stop_evt, 2) == WAIT_OBJECT_0)
            break;
        now_ms = (uint64_t)GetTickCount64();

        EnterCriticalSection(&g->mgr_cs);
        (void)vgd_ctrl_tick(g->mgr, now_ms, g->ops);
        LeaveCriticalSection(&g->mgr_cs);

        if (now_ms >= last_oem_sync_ms + 2000u) {
            last_oem_sync_ms = now_ms;
            vgd_win_sync_oem_names_if_needed(g->backend);
        }
    }
    return 0;
}

static DWORD WINAPI rumble_thread_proc(LPVOID param)
{
    RumbleWorker *w = (RumbleWorker *)param;
    for (;;) {
        uint8_t large_m = 0, small_m = 0;
        int rc = vgd_win_wait_rumble(w->backend, w->slot, w->stop_event, &large_m, &small_m);
        if (rc <= 0)
            break;
        {
            GamepadPacket pkt;
            uint32_t seq = (uint32_t)InterlockedIncrement(w->rumble_seq);
            gamepad_log("Rumble slot=%u large=%u small=%u", (unsigned)w->slot, (unsigned)large_m, (unsigned)small_m);
            if (vgd_ctrl_encode_rumble(w->slot, large_m, small_m, seq, &pkt)) {
                EnterCriticalSection(w->send_cs);
                if (w->conn)
                    (void)gp_send_all(w->conn, &pkt, (int)sizeof(pkt));
                LeaveCriticalSection(w->send_cs);
            }
        }
    }
    return 0;
}

static void start_rumble_worker(GuestGamepadCtx *g, uint8_t slot)
{
    RumbleWorker *w;
    if (slot >= GAMEPAD_MAX_SLOTS) return;
    w = &g->workers[slot];
    if (w->thread) return;
    memset(w, 0, sizeof(*w));
    w->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!w->stop_event) return;
    w->backend = g->backend;
    w->slot = slot;
    w->conn = g->conn;
    w->send_cs = &g->send_cs;
    w->rumble_seq = &g->rumble_seq;
    w->thread = CreateThread(NULL, 0, rumble_thread_proc, w, 0, NULL);
    if (!w->thread) {
        CloseHandle(w->stop_event);
        w->stop_event = NULL;
    }
}

static void stop_rumble_worker(GuestGamepadCtx *g, uint8_t slot)
{
    RumbleWorker *w;
    if (slot >= GAMEPAD_MAX_SLOTS) return;
    w = &g->workers[slot];
    if (w->stop_event) SetEvent(w->stop_event);
    if (w->thread) {
        WaitForSingleObject(w->thread, 2000);
        CloseHandle(w->thread);
        w->thread = NULL;
    }
    if (w->stop_event) {
        CloseHandle(w->stop_event);
        w->stop_event = NULL;
    }
}

int main(void)
{
    AsbListener *listener = NULL;
    VgdWinBackend backend;
    wchar_t exe_dir[MAX_PATH], drivers_dir[MAX_PATH];
    wchar_t *slash;

    gamepad_log("appsandbox-gamepad (UMDF2 VGD) starting.");
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    if (asb_transport_init() != 0) {
        gamepad_log("asb_transport_init failed.");
        timeEndPeriod(1);
        return 1;
    }

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';
    swprintf_s(drivers_dir, MAX_PATH, L"%s\\drivers", exe_dir);

    vgd_win_backend_init(&backend, drivers_dir, gamepad_log);
    vgd_win_backend_cleanup_stale(&backend);

    if (GetFileAttributesW(backend.xusb_inf_path) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(backend.hid_inf_path) == INVALID_FILE_ATTRIBUTES) {
        gamepad_log("Warning: AppSandboxVGD INF(s) not yet staged under %ls.", drivers_dir);
    }

    for (;;) {
        if (!listener) {
            listener = asb_listen(ASB_CH_GAMEPAD);
            if (!listener) {
                gamepad_log("asb_listen(ASB_CH_GAMEPAD) failed; retrying.");
                Sleep(2000);
                continue;
            }
            gamepad_log("Listening on ASB_CH_GAMEPAD (%d).", ASB_CH_GAMEPAD);
        }

        {
            AsbConn *conn = asb_accept(listener, 1000);
            uint32_t greeting = GAMEPAD_READY_MAGIC;
            GuestGamepadCtx ctx;
            VgdControllerMgr mgr;
            VgdBackendOps ops;
            uint8_t s;

            if (!conn)
                continue;

            gamepad_log("Host connected on Gamepad Channel.");
            if (gp_send_all(conn, &greeting, (int)sizeof(greeting)) != 0) {
                gamepad_log("Failed to send GRDY greeting.");
                asb_close(conn);
                continue;
            }

            memset(&ctx, 0, sizeof(ctx));
            vgd_ctrl_init(&mgr);
            memset(&ops, 0, sizeof(ops));
            ops.ctx = &backend;
            ops.plug_slot = vgd_win_plug_slot;
            ops.unplug_slot = vgd_win_unplug_slot;
            ops.write_state = vgd_win_write_state;
            ops.set_slot_name = vgd_win_set_slot_name;

            ctx.backend = &backend;
            ctx.mgr = &mgr;
            ctx.ops = &ops;
            ctx.conn = conn;
            InitializeCriticalSection(&ctx.send_cs);
            InitializeCriticalSection(&ctx.mgr_cs);
            ctx.flush_stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (ctx.flush_stop_evt) {
                ctx.flush_thread = CreateThread(NULL, 0, flush_thread_proc, &ctx, 0, NULL);
            }

            for (;;) {
                GamepadPacket pkt;
                int ok = 0;
                int start_rumble = 0;

                if (gp_recv_all(conn, &pkt, (int)sizeof(pkt)) != 0)
                    break;
                if (!asb_gamepad_packet_validate(&pkt))
                    continue;

                if (pkt.type == GAMEPAD_PKT_UNPLUG && mgr.plugged[pkt.slot])
                    stop_rumble_worker(&ctx, pkt.slot);

                EnterCriticalSection(&ctx.mgr_cs);
                if (pkt.type == GAMEPAD_PKT_STATE) {
                    ok = vgd_ctrl_apply_state_timed(&mgr, pkt.slot, &pkt.state,
                                                    (uint64_t)GetTickCount64(), &ops);
                } else {
                    ok = vgd_ctrl_handle_packet(&mgr, &pkt, &ops);
                }
                if (ok && (pkt.type == GAMEPAD_PKT_PLUG || pkt.type == GAMEPAD_PKT_STATE) &&
                    mgr.plugged[pkt.slot] && !ctx.workers[pkt.slot].thread) {
                    start_rumble = 1;
                }
                LeaveCriticalSection(&ctx.mgr_cs);

                if (start_rumble) {
                    start_rumble_worker(&ctx, pkt.slot);
                }
            }

            gamepad_log("Host disconnected; unplugging active virtual controllers.");
            if (ctx.flush_stop_evt) SetEvent(ctx.flush_stop_evt);
            if (ctx.flush_thread) {
                WaitForSingleObject(ctx.flush_thread, 2000);
                CloseHandle(ctx.flush_thread);
                ctx.flush_thread = NULL;
            }
            if (ctx.flush_stop_evt) {
                CloseHandle(ctx.flush_stop_evt);
                ctx.flush_stop_evt = NULL;
            }
            for (s = 0; s < GAMEPAD_MAX_SLOTS; s++)
                stop_rumble_worker(&ctx, s);
            EnterCriticalSection(&ctx.mgr_cs);
            vgd_ctrl_unplug_all(&mgr, &ops);
            LeaveCriticalSection(&ctx.mgr_cs);

            EnterCriticalSection(&ctx.send_cs);
            ctx.conn = NULL;
            LeaveCriticalSection(&ctx.send_cs);
            DeleteCriticalSection(&ctx.mgr_cs);
            DeleteCriticalSection(&ctx.send_cs);
            asb_close(conn);
        }
    }
}
