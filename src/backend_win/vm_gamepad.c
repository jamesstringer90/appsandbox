/*
 * vm_gamepad.c -- Host-side XInput controller poller + AF_HYPERV Gamepad Channel.
 *
 * Connects to the guest appsandbox-gamepad helper on ASB_CH_GAMEPAD (channel 8),
 * waits for GAMEPAD_READY_MAGIC ("GRDY"), polls host Xbox controllers (slots 0..3)
 * via dynamically loaded xinput1_4.dll, enforces Focus-Gated Forwarding, and plays
 * back guest-to-host Haptic Feedback via XInputSetState.
 *
 * Pure C, compiled as C (/W4).
 */

#include "vm_gamepad.h"
#include "../../tools/transport/asb_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================================================================
 * Pure-C Host Gamepad State Machine (testable without hardware/sockets)
 * ================================================================== */

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-010
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-011
void vm_gamepad_core_init(VmGamepadCore *core, int forwarding_enabled)
{
    if (!core) return;
    memset(core, 0, sizeof(*core));
    core->forwarding_enabled = forwarding_enabled ? 1 : 0;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-004
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-005
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-008
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-010
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-011
// @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-023
void vm_gamepad_core_step(VmGamepadCore *core, const VmGamepadOps *ops)
{
    uint8_t slot;
    uint64_t now;

    if (!core || !ops || !ops->read_slot || !ops->send_packet)
        return;

    now = ops->now_ms ? ops->now_ms(ops->ctx) : 0;

    for (slot = 0; slot < GAMEPAD_MAX_SLOTS; slot++) {
        GamepadState cur;
        int present;

        memset(&cur, 0, sizeof(cur));
        present = ops->read_slot(ops->ctx, slot, &cur) ? 1 : 0;

        if (present && !core->connected[slot]) {
            GamepadPacket pkt;
            char name_buf[GAMEPAD_NAME_MAX_LEN];
            int has_name = 0;

            core->connected[slot] = 1;
            memset(name_buf, 0, sizeof(name_buf));
            if (ops->read_name &&
                ops->read_name(ops->ctx, slot, name_buf, sizeof(name_buf)) > 0 &&
                name_buf[0] != '\0') {
                size_t nlen = strlen(name_buf);
                size_t max_bytes = (size_t)GAMEPAD_NAME_CHUNK_BYTES * (size_t)GAMEPAD_NAME_MAX_CHUNKS;
                uint8_t total_chunks, ci;
                if (nlen > max_bytes) nlen = max_bytes;
                total_chunks = (uint8_t)((nlen + GAMEPAD_NAME_CHUNK_BYTES - 1u) / GAMEPAD_NAME_CHUNK_BYTES);
                for (ci = 0; ci < total_chunks; ci++) {
                    char slice[GAMEPAD_NAME_CHUNK_BYTES + 1];
                    size_t off = (size_t)ci * GAMEPAD_NAME_CHUNK_BYTES;
                    size_t rem = nlen - off;
                    size_t take = rem > GAMEPAD_NAME_CHUNK_BYTES ? GAMEPAD_NAME_CHUNK_BYTES : rem;
                    memset(slice, 0, sizeof(slice));
                    memcpy(slice, name_buf + off, take);
                    if (asb_gamepad_make_name_chunk(slot, ci, total_chunks, slice, ++core->seq, &pkt)) {
                        ops->send_packet(ops->ctx, &pkt);
                    }
                }
                has_name = 1;
            }
            if (ops->on_log) {
                wchar_t msg[160];
                if (has_name) {
                    wchar_t wname[GAMEPAD_NAME_MAX_LEN];
                    size_t i = 0;
                    while (name_buf[i] != '\0' && i + 1u < GAMEPAD_NAME_MAX_LEN) {
                        wname[i] = (wchar_t)(unsigned char)name_buf[i];
                        i++;
                    }
                    wname[i] = L'\0';
                    swprintf_s(msg, sizeof(msg) / sizeof(msg[0]),
                               L"Gamepad: host controller connected on slot %u (%ls).",
                               (unsigned)slot, wname);
                } else {
                    swprintf_s(msg, sizeof(msg) / sizeof(msg[0]),
                               L"Gamepad: host controller connected on slot %u.", (unsigned)slot);
                }
                ops->on_log(ops->ctx, msg);
            }
            asb_gamepad_make_plug(slot, ++core->seq, &pkt);
            ops->send_packet(ops->ctx, &pkt);

            if (core->forwarding_enabled) {
                asb_gamepad_make_state(slot, &cur, ++core->seq, &pkt);
                ops->send_packet(ops->ctx, &pkt);
                core->last_sent[slot] = cur;
                core->last_send_ms[slot] = now;
                core->dirty[slot] = 0;
            } else {
                memset(&core->last_sent[slot], 0, sizeof(GamepadState));
                core->last_send_ms[slot] = now;
                core->dirty[slot] = 1;
            }
            continue;
        }

        if (!present && core->connected[slot]) {
            GamepadPacket pkt;
            core->connected[slot] = 0;
            core->dirty[slot] = 0;
            memset(&core->last_sent[slot], 0, sizeof(GamepadState));
            if (ops->on_log) {
                wchar_t msg[96];
                swprintf_s(msg, sizeof(msg) / sizeof(msg[0]),
                           L"Gamepad: host controller disconnected from slot %u.", (unsigned)slot);
                ops->on_log(ops->ctx, msg);
            }
            if (ops->set_rumble)
                ops->set_rumble(ops->ctx, slot, 0, 0);
            asb_gamepad_make_unplug(slot, ++core->seq, &pkt);
            ops->send_packet(ops->ctx, &pkt);
            continue;
        }

        if (present && core->connected[slot] && core->forwarding_enabled) {
            int changed = core->dirty[slot] || !asb_gamepad_state_equal(&cur, &core->last_sent[slot]);
            int keepalive = (now >= core->last_send_ms[slot]) &&
                            ((now - core->last_send_ms[slot]) >= VM_GAMEPAD_KEEPALIVE_MS);
            if (changed || keepalive) {
                GamepadPacket pkt;
                asb_gamepad_make_state(slot, &cur, ++core->seq, &pkt);
                ops->send_packet(ops->ctx, &pkt);
                core->last_sent[slot] = cur;
                core->last_send_ms[slot] = now;
                core->dirty[slot] = 0;
            }
        }
    }
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-007
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-008
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-011
void vm_gamepad_core_set_forwarding(VmGamepadCore *core, int enabled, const VmGamepadOps *ops)
{
    uint8_t slot;
    int next = enabled ? 1 : 0;

    if (!core) return;
    if (core->forwarding_enabled == next) return;
    core->forwarding_enabled = next;

    if (!next) {
        for (slot = 0; slot < GAMEPAD_MAX_SLOTS; slot++) {
            if (!core->connected[slot]) continue;
            if (ops && ops->send_packet) {
                GamepadPacket pkt;
                asb_gamepad_make_neutral(slot, &pkt);
                pkt.seq = ++core->seq;
                ops->send_packet(ops->ctx, &pkt);
            }
            memset(&core->last_sent[slot], 0, sizeof(GamepadState));
            core->dirty[slot] = 1;
            if (ops && ops->set_rumble)
                ops->set_rumble(ops->ctx, slot, 0, 0);
        }
    } else {
        for (slot = 0; slot < GAMEPAD_MAX_SLOTS; slot++) {
            if (core->connected[slot])
                core->dirty[slot] = 1;
        }
    }
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-006
int vm_gamepad_core_handle_rx(VmGamepadCore *core, const GamepadPacket *pkt, const VmGamepadOps *ops)
{
    if (!core || !pkt)
        return 0;
    if (!asb_gamepad_packet_validate(pkt))
        return 0;

    if (pkt->type == GAMEPAD_PKT_RUMBLE) {
        if (core->forwarding_enabled && core->connected[pkt->slot] && ops && ops->set_rumble)
            ops->set_rumble(ops->ctx, pkt->slot, pkt->large_motor, pkt->small_motor);
        return 1;
    }
    return 1;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-008
void vm_gamepad_core_on_disconnect(VmGamepadCore *core, const VmGamepadOps *ops)
{
    uint8_t slot;
    if (!core) return;
    for (slot = 0; slot < GAMEPAD_MAX_SLOTS; slot++) {
        if (core->connected[slot] && ops && ops->set_rumble)
            ops->set_rumble(ops->ctx, slot, 0, 0);
        core->connected[slot] = 0;
        core->dirty[slot] = 0;
        memset(&core->last_sent[slot], 0, sizeof(GamepadState));
    }
}

/* ==================================================================
 * Win32 Host Runtime Adapter (dynamic xinput1_4.dll + AF_HYPERV)
 * ================================================================== */

#ifdef _WIN32
#include <mmsystem.h>
#include <setupapi.h>
#include <winioctl.h>
#include <hidclass.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "setupapi.lib")

static const GUID k_guid_devinterface_hid = {
    0x4D1E55B2u, 0xF16Fu, 0x11CFu,
    { 0x88u, 0xCBu, 0x00u, 0x11u, 0x11u, 0x00u, 0x00u, 0x30u }
};

#ifndef AF_HYPERV
#define AF_HYPERV 34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif

typedef struct _ASB_SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT         Reserved;
    GUID           VmId;
    GUID           ServiceId;
} ASB_SOCKADDR_HV;

typedef struct _ASB_XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} ASB_XINPUT_GAMEPAD;

typedef struct _ASB_XINPUT_STATE_EX {
    DWORD              dwPacketNumber;
    ASB_XINPUT_GAMEPAD Gamepad;
    DWORD              dwReserved;
} ASB_XINPUT_STATE_EX;

typedef struct _ASB_XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} ASB_XINPUT_VIBRATION;

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, ASB_XINPUT_STATE_EX *pState);
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD dwUserIndex, ASB_XINPUT_VIBRATION *pVibration);

typedef struct XInputApi {
    HMODULE            module;
    PFN_XInputGetState get_state;
    PFN_XInputSetState set_state;
} XInputApi;

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-003
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-013
static BOOL xinput_load(XInputApi *api)
{
    static const wchar_t *dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    size_t i;

    memset(api, 0, sizeof(*api));
    for (i = 0; i < ARRAYSIZE(dlls); i++) {
        HMODULE mod = LoadLibraryW(dlls[i]);
        if (!mod) continue;
        /* Ordinal 100 is XInputGetStateEx (exposes Guide button bit 0x0400); fall back to XInputGetState. */
        api->get_state = (PFN_XInputGetState)GetProcAddress(mod, (LPCSTR)100);
        if (!api->get_state)
            api->get_state = (PFN_XInputGetState)GetProcAddress(mod, "XInputGetState");
        api->set_state = (PFN_XInputSetState)GetProcAddress(mod, "XInputSetState");
        if (api->get_state) {
            api->module = mod;
            return TRUE;
        }
        FreeLibrary(mod);
    }
    return FALSE;
}

static void xinput_unload(XInputApi *api)
{
    if (api && api->module) {
        FreeLibrary(api->module);
        memset(api, 0, sizeof(*api));
    }
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-003
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-013
BOOL vm_gamepad_xinput_available(void)
{
    XInputApi api;
    BOOL ok = xinput_load(&api);
    xinput_unload(&api);
    return ok;
}

static void gamepad_service_guid(const wchar_t *os_type, GUID *out)
{
    BOOL is_windows = !os_type || os_type[0] == L'\0' || _wcsicmp(os_type, L"Windows") == 0;
    if (is_windows) {
        out->Data1 = 0xa5b0cafeu;
        out->Data2 = (unsigned short)ASB_CH_GAMEPAD;
        out->Data3 = 0x4000;
        out->Data4[0] = 0x80; out->Data4[1] = 0x00;
        out->Data4[2] = 0x00; out->Data4[3] = 0x00;
        out->Data4[4] = 0x00; out->Data4[5] = 0x00;
        out->Data4[6] = 0x00; out->Data4[7] = 0x01;
    } else {
        out->Data1 = (unsigned long)ASB_CH_GAMEPAD;
        out->Data2 = 0xFACB;
        out->Data3 = 0x11E6;
        out->Data4[0] = 0xBD; out->Data4[1] = 0x58;
        out->Data4[2] = 0x64; out->Data4[3] = 0x00;
        out->Data4[4] = 0x6A; out->Data4[5] = 0x79;
        out->Data4[6] = 0x86; out->Data4[7] = 0xD3;
    }
}

struct VmGamepad {
    GUID             vm_guid;
    wchar_t          os_type[32];
    CRITICAL_SECTION lock;
    volatile LONG    stop;
    volatile SOCKET  sock;
    volatile LONG    connected_flag;
    HANDLE           worker_thread;
    XInputApi        xinput;
    VmGamepadCore    core;
    VmGamepadOps     ops;
    VmGamepadLogFn   log_fn;
    void            *log_ctx;
};

static int send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int left = len;
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n <= 0) return 0;
        p += n;
        left -= n;
    }
    return 1;
}

static int recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int left = len;
    while (left > 0) {
        int n = recv(s, p, left, 0);
        if (n <= 0) return 0;
        p += n;
        left -= n;
    }
    return 1;
}

static int win_read_slot(void *ctx, uint8_t slot, GamepadState *out_state)
{
    VmGamepad *gp = (VmGamepad *)ctx;
    ASB_XINPUT_STATE_EX xs;
    if (!gp->xinput.get_state || slot >= GAMEPAD_MAX_SLOTS)
        return 0;
    memset(&xs, 0, sizeof(xs));
    if (gp->xinput.get_state((DWORD)slot, &xs) != ERROR_SUCCESS)
        return 0;
    out_state->buttons = xs.Gamepad.wButtons;
    out_state->left_trigger = xs.Gamepad.bLeftTrigger;
    out_state->right_trigger = xs.Gamepad.bRightTrigger;
    out_state->thumb_lx = xs.Gamepad.sThumbLX;
    out_state->thumb_ly = xs.Gamepad.sThumbLY;
    out_state->thumb_rx = xs.Gamepad.sThumbRX;
    out_state->thumb_ry = xs.Gamepad.sThumbRY;
    return 1;
}

static int wcs_contains_ci(const wchar_t *haystack, const wchar_t *needle)
{
    size_t nlen, i;
    if (!haystack || !needle || !needle[0]) return 0;
    nlen = wcslen(needle);
    for (; *haystack; haystack++) {
        int match = 1;
        for (i = 0; i < nlen; i++) {
            wchar_t a = haystack[i];
            wchar_t b = needle[i];
            if (a == L'\0') return 0;
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

// @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-023
static int win_read_name(void *ctx, uint8_t slot, char *utf8_out, size_t cap)
{
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA if_data;
    DWORD idx;
    DWORD seen_insts[16];
    DWORD seen_count = 0;
    uint8_t match_idx = 0;

    (void)ctx;
    if (!utf8_out || cap < 2u || slot >= GAMEPAD_MAX_SLOTS)
        return 0;
    utf8_out[0] = '\0';

    devs = SetupDiGetClassDevsW(&k_guid_devinterface_hid, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE)
        return 0;

    if_data.cbSize = sizeof(if_data);
    for (idx = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &k_guid_devinterface_hid, idx, &if_data); idx++) {
        DWORD req_bytes = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        SP_DEVINFO_DATA dev_info;
        DWORD j;
        int dup = 0;

        SetupDiGetDeviceInterfaceDetailW(devs, &if_data, NULL, 0, &req_bytes, NULL);
        if (req_bytes == 0) continue;

        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(req_bytes);
        if (!detail) continue;
        detail->cbSize = sizeof(*detail);
        memset(&dev_info, 0, sizeof(dev_info));
        dev_info.cbSize = sizeof(dev_info);

        if (!SetupDiGetDeviceInterfaceDetailW(devs, &if_data, detail, req_bytes, NULL, &dev_info)) {
            free(detail);
            continue;
        }

        if (!wcs_contains_ci(detail->DevicePath, L"ig_")) {
            free(detail);
            continue;
        }

        for (j = 0; j < seen_count; j++) {
            if (seen_insts[j] == dev_info.DevInst) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            free(detail);
            continue;
        }
        if (seen_count < ARRAYSIZE(seen_insts))
            seen_insts[seen_count++] = dev_info.DevInst;

        if (match_idx == slot) {
            wchar_t wname[126];
            HANDLE h;
            int got = 0;

            memset(wname, 0, sizeof(wname));
            h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD bytes = 0;
                if (DeviceIoControl(h, IOCTL_HID_GET_PRODUCT_STRING, NULL, 0,
                                    wname, (DWORD)(sizeof(wname) - sizeof(wchar_t)), &bytes, NULL) &&
                    wname[0] != L'\0') {
                    wname[ARRAYSIZE(wname) - 1] = L'\0';
                    got = 1;
                }
                CloseHandle(h);
            }

            if (!got) {
                DWORD reg_type = 0, prop_bytes = 0;
                if (SetupDiGetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_FRIENDLYNAME,
                                                      &reg_type, (PBYTE)wname,
                                                      (DWORD)(sizeof(wname) - sizeof(wchar_t)),
                                                      &prop_bytes) &&
                    wname[0] != L'\0') {
                    wname[ARRAYSIZE(wname) - 1] = L'\0';
                    got = 1;
                } else if (SetupDiGetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_DEVICEDESC,
                                                             &reg_type, (PBYTE)wname,
                                                             (DWORD)(sizeof(wname) - sizeof(wchar_t)),
                                                             &prop_bytes) &&
                           wname[0] != L'\0') {
                    wname[ARRAYSIZE(wname) - 1] = L'\0';
                    got = 1;
                }
            }

            free(detail);
            SetupDiDestroyDeviceInfoList(devs);

            if (got) {
                int n = WideCharToMultiByte(CP_UTF8, 0, wname, -1, utf8_out, (int)cap, NULL, NULL);
                if (n > 1) {
                    utf8_out[cap - 1u] = '\0';
                    return n - 1;
                }
            }
            return 0;
        }

        match_idx++;
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return 0;
}

static int win_send_packet(void *ctx, const GamepadPacket *pkt)
{
    VmGamepad *gp = (VmGamepad *)ctx;
    SOCKET s = gp->sock;
    if (s == INVALID_SOCKET)
        return 0;
    if (!send_all(s, pkt, (int)sizeof(*pkt))) {
        shutdown(s, SD_BOTH);
        return 0;
    }
    return 1;
}

static void win_set_rumble(void *ctx, uint8_t slot, uint8_t large_motor, uint8_t small_motor)
{
    VmGamepad *gp = (VmGamepad *)ctx;
    ASB_XINPUT_VIBRATION vib;
    if (!gp->xinput.set_state || slot >= GAMEPAD_MAX_SLOTS)
        return;
    vib.wLeftMotorSpeed  = (WORD)((unsigned)large_motor * 257u);
    vib.wRightMotorSpeed = (WORD)((unsigned)small_motor * 257u);
    gp->xinput.set_state((DWORD)slot, &vib);
}

static uint64_t win_now_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)GetTickCount64();
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-004::REQ-015
static void win_on_log(void *ctx, const wchar_t *msg)
{
    VmGamepad *gp = (VmGamepad *)ctx;
    if (gp && gp->log_fn && msg)
        gp->log_fn(gp->log_ctx, msg);
}

static DWORD WINAPI gamepad_rx_thread(LPVOID param)
{
    VmGamepad *gp = (VmGamepad *)param;
    while (!InterlockedCompareExchange(&gp->stop, 0, 0)) {
        GamepadPacket pkt;
        SOCKET s = gp->sock;
        if (s == INVALID_SOCKET) break;
        if (!recv_all(s, &pkt, (int)sizeof(pkt)))
            break;
        EnterCriticalSection(&gp->lock);
        vm_gamepad_core_handle_rx(&gp->core, &pkt, &gp->ops);
        LeaveCriticalSection(&gp->lock);
    }
    InterlockedExchange(&gp->connected_flag, 0);
    return 0;
}

static DWORD WINAPI gamepad_worker_thread(LPVOID param)
{
    VmGamepad *gp = (VmGamepad *)param;
    GUID svc_guid;

    gamepad_service_guid(gp->os_type, &svc_guid);

    while (!InterlockedCompareExchange(&gp->stop, 0, 0)) {
        SOCKET s;
        ASB_SOCKADDR_HV addr;
        uint32_t greeting = 0;
        DWORD timeout_ms = 5000;
        HANDLE rx_thread = NULL;

        s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        if (s == INVALID_SOCKET) {
            Sleep(1000);
            continue;
        }

        memset(&addr, 0, sizeof(addr));
        addr.Family = AF_HYPERV;
        addr.VmId = gp->vm_guid;
        addr.ServiceId = svc_guid;

        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            closesocket(s);
            Sleep(1000);
            continue;
        }

        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
        if (!recv_all(s, &greeting, (int)sizeof(greeting)) || greeting != GAMEPAD_READY_MAGIC) {
            closesocket(s);
            Sleep(1000);
            continue;
        }
        timeout_ms = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

        EnterCriticalSection(&gp->lock);
        gp->sock = s;
        InterlockedExchange(&gp->connected_flag, 1);
        LeaveCriticalSection(&gp->lock);
        win_on_log(gp, L"Gamepad: channel connected to guest.");

        rx_thread = CreateThread(NULL, 0, gamepad_rx_thread, gp, 0, NULL);

        /* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-018 */
        /* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-018 */
        timeBeginPeriod(1);
        while (!InterlockedCompareExchange(&gp->stop, 0, 0) &&
               InterlockedCompareExchange(&gp->connected_flag, 0, 0)) {
            EnterCriticalSection(&gp->lock);
            vm_gamepad_core_step(&gp->core, &gp->ops);
            LeaveCriticalSection(&gp->lock);
            Sleep(2);
        }
        timeEndPeriod(1);

        EnterCriticalSection(&gp->lock);
        if (gp->sock != INVALID_SOCKET) {
            shutdown(gp->sock, SD_BOTH);
            closesocket(gp->sock);
            gp->sock = INVALID_SOCKET;
        }
        vm_gamepad_core_on_disconnect(&gp->core, &gp->ops);
        LeaveCriticalSection(&gp->lock);
        if (!InterlockedCompareExchange(&gp->stop, 0, 0))
            win_on_log(gp, L"Gamepad: channel disconnected from guest.");

        if (rx_thread) {
            WaitForSingleObject(rx_thread, 2000);
            CloseHandle(rx_thread);
        }

        if (!InterlockedCompareExchange(&gp->stop, 0, 0))
            Sleep(1000);
    }

    return 0;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-003
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-013
// @implements spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-004::REQ-015
VmGamepad *vm_gamepad_start(const GUID *vm_guid, const wchar_t *os_type,
                            BOOL initial_forwarding, VmGamepadLogFn log_fn, void *log_ctx)
{
    VmGamepad *gp;

    if (!vm_guid) return NULL;
    gp = (VmGamepad *)calloc(1, sizeof(VmGamepad));
    if (!gp) return NULL;

    gp->log_fn = log_fn;
    gp->log_ctx = log_ctx;

    if (!xinput_load(&gp->xinput)) {
        win_on_log(gp, L"Warning: xinput1_4.dll unavailable; gamepad forwarding disabled.");
        OutputDebugStringW(L"[AppSandbox] Warning: xinput1_4.dll unavailable; gamepad forwarding disabled.\n");
        free(gp);
        return NULL;
    }

    gp->vm_guid = *vm_guid;
    if (os_type && os_type[0])
        wcsncpy_s(gp->os_type, ARRAYSIZE(gp->os_type), os_type, _TRUNCATE);
    else
        wcscpy_s(gp->os_type, ARRAYSIZE(gp->os_type), L"Windows");

    gp->sock = INVALID_SOCKET;
    InitializeCriticalSection(&gp->lock);

    gp->ops.ctx = gp;
    gp->ops.read_slot = win_read_slot;
    gp->ops.send_packet = win_send_packet;
    gp->ops.set_rumble = win_set_rumble;
    gp->ops.now_ms = win_now_ms;
    gp->ops.on_log = win_on_log;
    gp->ops.read_name = win_read_name;

    vm_gamepad_core_init(&gp->core, initial_forwarding ? 1 : 0);

    gp->worker_thread = CreateThread(NULL, 0, gamepad_worker_thread, gp, 0, NULL);
    if (!gp->worker_thread) {
        DeleteCriticalSection(&gp->lock);
        xinput_unload(&gp->xinput);
        free(gp);
        return NULL;
    }
    return gp;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-007
// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-008
void vm_gamepad_set_forwarding(VmGamepad *gp, BOOL enabled)
{
    if (!gp) return;
    EnterCriticalSection(&gp->lock);
    vm_gamepad_core_set_forwarding(&gp->core, enabled ? 1 : 0, &gp->ops);
    LeaveCriticalSection(&gp->lock);
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-008
void vm_gamepad_stop(VmGamepad *gp)
{
    if (!gp) return;
    InterlockedExchange(&gp->stop, 1);

    EnterCriticalSection(&gp->lock);
    if (gp->sock != INVALID_SOCKET)
        shutdown(gp->sock, SD_BOTH);
    LeaveCriticalSection(&gp->lock);

    if (gp->worker_thread) {
        WaitForSingleObject(gp->worker_thread, 3000);
        CloseHandle(gp->worker_thread);
        gp->worker_thread = NULL;
    }

    EnterCriticalSection(&gp->lock);
    vm_gamepad_core_on_disconnect(&gp->core, &gp->ops);
    if (gp->sock != INVALID_SOCKET) {
        closesocket(gp->sock);
        gp->sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&gp->lock);

    DeleteCriticalSection(&gp->lock);
    xinput_unload(&gp->xinput);
    free(gp);
}

#endif /* _WIN32 */
