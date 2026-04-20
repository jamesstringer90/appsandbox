/*
 * asb_display.c -- Headless IDD display for programmatic VM interaction.
 *
 * Connects to a running VM's AF_HYPERV frame and input channels,
 * receives frames into a CPU-side buffer, and sends keyboard/mouse input.
 *
 * This is a stripped-down version of vm_display_idd.c — no D3D11, no window,
 * no clipboard, no cursor rendering.
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#include "asb_display.h"
#include "asb_core.h"
#include "hcs_vm.h"
#include "ui.h"

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

static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Frame protocol ---- */

#define FRAME_MAGIC         0x52465341  /* "ASFR" */
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MAX_DIRTY_RECTS     64
#define MAX_FRAME_DATA_SIZE (DEFAULT_WIDTH * DEFAULT_HEIGHT * 4)

/* ---- Input protocol ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3

#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" */

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 type;
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;

typedef struct {
    UINT32 magic;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT64 frame_seq;
    UINT32 dirty_rect_count;
} FrameHeader;

/* Cursor header — we skip cursor data in headless mode */
#define CURSOR_MAGIC        0x52435341  /* "ASCR" */
#define MAX_CURSOR_SIZE     (256 * 256 * 4 * 2)

typedef struct {
    UINT32 magic;
    INT32  x;
    INT32  y;
    UINT32 visible;
    UINT32 shape_updated;
    UINT32 shape_id;
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 xhot;
    UINT32 yhot;
    UINT32 cursor_type;
    UINT32 shape_data_size;
} CursorHeader;
#pragma pack(pop)

/* ---- Display context ---- */

struct AsbDisplay {
    VmInstance      *vm;
    GUID             runtime_id;
    BYTE            *frame_buf;
    UINT             frame_width;
    UINT             frame_height;
    UINT             frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL    frame_dirty;
    volatile BOOL    has_frame;
    volatile SOCKET  input_socket;
    HANDLE           recv_thread;
    volatile BOOL    stop;
    volatile BOOL    connected;
};

/* ---- Socket helpers ---- */

static BOOL recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

static SOCKET connect_hv_service(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
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
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ---- Send input packet ---- */

static void send_input(AsbDisplay *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    SOCKET s;

    s = d->input_socket;
    if (s == INVALID_SOCKET) return;

    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    if (send(s, (const char *)&pkt, (int)sizeof(pkt), 0) == SOCKET_ERROR) {
        d->input_socket = INVALID_SOCKET;
    }
}

/* ---- Frame receive thread ---- */

static DWORD WINAPI display_recv_thread(LPVOID param)
{
    AsbDisplay *d = (AsbDisplay *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;
    SOCKET input_s = INVALID_SOCKET;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, MAX_FRAME_DATA_SIZE);
    if (!recv_buf) return 1;

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;

        /* Connect input channel */
        if (input_s == INVALID_SOCKET) {
            input_s = connect_hv_service(&d->runtime_id, &INPUT_SERVICE_GUID, 1000);
            if (input_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(input_s, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == INPUT_READY_MAGIC) {
                    DWORD zero_timeout = 0;
                    u_long nb = 1;
                    setsockopt(input_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                    ioctlsocket(input_s, FIONBIO, &nb);
                    d->input_socket = input_s;
                } else {
                    closesocket(input_s);
                    input_s = INVALID_SOCKET;
                }
            }
        }

        /* Connect frame channel */
        s = connect_hv_service(&d->runtime_id, &FRAME_SERVICE_GUID, 3000);
        if (s == INVALID_SOCKET) {
            int wait;
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        d->connected = TRUE;

        /* Receive loop */
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size, rect_count, i, magic;

            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            /* Skip cursor messages */
            if (magic == CURSOR_MAGIC) {
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;
                if (chdr.shape_updated && chdr.shape_data_size > 0) {
                    if (chdr.shape_data_size > MAX_CURSOR_SIZE) break;
                    /* Skip cursor bitmap data */
                    {
                        BYTE *skip = (BYTE *)HeapAlloc(GetProcessHeap(), 0, chdr.shape_data_size);
                        if (!skip) break;
                        if (!recv_exact(s, skip, (int)chdr.shape_data_size)) {
                            HeapFree(GetProcessHeap(), 0, skip);
                            break;
                        }
                        HeapFree(GetProcessHeap(), 0, skip);
                    }
                }
                continue;
            }

            if (magic != FRAME_MAGIC) break;

            hdr.magic = magic;
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
                break;

            if (hdr.width == 0 || hdr.height == 0 ||
                hdr.width > 7680 || hdr.height > 4320 ||
                hdr.stride < hdr.width * 4)
                break;

            rect_count = hdr.dirty_rect_count;
            if (rect_count > MAX_DIRTY_RECTS) break;

            if (rect_count > 0) {
                if (!recv_exact(s, dirty_rects, (int)(rect_count * sizeof(RECT))))
                    break;
            }

            if (!recv_exact(s, &data_size, 4))
                break;

            if (data_size > MAX_FRAME_DATA_SIZE) break;

            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            if (hdr.width != d->frame_width || hdr.height != d->frame_height) {
                UINT new_stride = hdr.width * 4;
                UINT new_size   = new_stride * hdr.height;
                BYTE *new_buf   = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_size);
                if (new_buf) {
                    if (d->frame_buf) HeapFree(GetProcessHeap(), 0, d->frame_buf);
                    d->frame_buf    = new_buf;
                    d->frame_width  = hdr.width;
                    d->frame_height = hdr.height;
                    d->frame_stride = new_stride;
                } else {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
            }

            if (rect_count == 0) {
                /* Full frame update */
                UINT row;
                UINT copy_w = hdr.width * 4;
                BYTE *src = recv_buf;
                if (copy_w > d->frame_stride) copy_w = d->frame_stride;
                if (copy_w > hdr.stride)      copy_w = hdr.stride;
                for (row = 0; row < hdr.height && row < d->frame_height; row++) {
                    memcpy(d->frame_buf + row * d->frame_stride,
                           src + row * hdr.stride, copy_w);
                }
            } else {
                /* Dirty rect updates */
                BYTE *src = recv_buf;
                for (i = 0; i < rect_count; i++) {
                    LONG left   = dirty_rects[i].left;
                    LONG top    = dirty_rects[i].top;
                    LONG right  = dirty_rects[i].right;
                    LONG bottom = dirty_rects[i].bottom;
                    UINT rect_w, rect_h, row;
                    UINT rect_row_bytes;

                    if (left < 0) left = 0;
                    if (top  < 0) top  = 0;
                    if (right  > (LONG)d->frame_width)  right  = (LONG)d->frame_width;
                    if (bottom > (LONG)d->frame_height) bottom = (LONG)d->frame_height;
                    if (left >= right || top >= bottom) continue;

                    rect_w = (UINT)(right - left);
                    rect_h = (UINT)(bottom - top);
                    rect_row_bytes = rect_w * 4;

                    for (row = 0; row < rect_h; row++) {
                        UINT dst_y = (UINT)top + row;
                        memcpy(d->frame_buf + dst_y * d->frame_stride + (UINT)left * 4,
                               src + row * rect_row_bytes, rect_row_bytes);
                    }
                    src += rect_row_bytes * rect_h;
                }
            }

            d->frame_dirty = TRUE;
            d->has_frame = TRUE;
            LeaveCriticalSection(&d->frame_cs);

            /* Reconnect input if flagged dead */
            if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
            }
            if (input_s == INVALID_SOCKET) {
                SOCKET new_s = connect_hv_service(&d->runtime_id, &INPUT_SERVICE_GUID, 1000);
                if (new_s != INVALID_SOCKET) {
                    UINT32 ready_magic = 0;
                    if (recv_exact(new_s, &ready_magic, sizeof(ready_magic)) &&
                        ready_magic == INPUT_READY_MAGIC) {
                        DWORD zero_timeout = 0;
                        u_long nb = 1;
                        setsockopt(new_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                        ioctlsocket(new_s, FIONBIO, &nb);
                        input_s = new_s;
                        d->input_socket = new_s;
                    } else {
                        closesocket(new_s);
                    }
                }
            }
        }

        closesocket(s);
        d->connected = FALSE;
    }

    /* Cleanup */
    if (input_s != INVALID_SOCKET) closesocket(input_s);
    d->input_socket = INVALID_SOCKET;
    if (recv_buf) HeapFree(GetProcessHeap(), 0, recv_buf);
    WSACleanup();
    return 0;
}

/* ================================================================
 * Public API
 * ================================================================ */

ASB_API AsbDisplay *asb_display_connect(AsbVm vm)
{
    VmInstance *inst = asb_vm_instance(vm);
    AsbDisplay *d;
    static const GUID zero_guid = {0};

    if (!inst || !inst->running) return NULL;
    if (memcmp(&inst->runtime_id, &zero_guid, sizeof(GUID)) == 0) {
        /* Try to find RuntimeId */
        if (!hcs_find_runtime_id(inst->name, &inst->runtime_id))
            return NULL;
    }

    d = (AsbDisplay *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AsbDisplay));
    if (!d) return NULL;

    d->vm = inst;
    d->runtime_id = inst->runtime_id;
    d->input_socket = INVALID_SOCKET;
    InitializeCriticalSection(&d->frame_cs);

    d->recv_thread = CreateThread(NULL, 0, display_recv_thread, d, 0, NULL);
    if (!d->recv_thread) {
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

ASB_API void asb_display_disconnect(AsbDisplay *disp)
{
    if (!disp) return;

    disp->stop = TRUE;
    if (disp->recv_thread) {
        WaitForSingleObject(disp->recv_thread, 5000);
        CloseHandle(disp->recv_thread);
    }

    EnterCriticalSection(&disp->frame_cs);
    if (disp->frame_buf) {
        HeapFree(GetProcessHeap(), 0, disp->frame_buf);
        disp->frame_buf = NULL;
    }
    LeaveCriticalSection(&disp->frame_cs);

    DeleteCriticalSection(&disp->frame_cs);
    HeapFree(GetProcessHeap(), 0, disp);
}

ASB_API BOOL asb_display_is_connected(AsbDisplay *disp)
{
    return disp ? disp->connected : FALSE;
}

ASB_API BOOL asb_display_screenshot(AsbDisplay *disp, BYTE *buf_out,
                                     UINT *width, UINT *height, UINT *stride)
{
    if (!disp) return FALSE;

    EnterCriticalSection(&disp->frame_cs);

    if (!disp->has_frame || !disp->frame_buf) {
        LeaveCriticalSection(&disp->frame_cs);
        if (width)  *width  = 0;
        if (height) *height = 0;
        if (stride) *stride = 0;
        return FALSE;
    }

    if (width)  *width  = disp->frame_width;
    if (height) *height = disp->frame_height;
    if (stride) *stride = disp->frame_stride;

    if (buf_out) {
        memcpy(buf_out, disp->frame_buf, disp->frame_stride * disp->frame_height);
        disp->frame_dirty = FALSE;
    }

    LeaveCriticalSection(&disp->frame_cs);
    return TRUE;
}

/* ---- Mouse input ---- */

ASB_API void asb_display_mouse_move(AsbDisplay *disp, UINT x, UINT y)
{
    if (!disp) return;
    send_input(disp, INPUT_MOUSE_MOVE, x, y, 0);
}

ASB_API void asb_display_mouse_click(AsbDisplay *disp, UINT x, UINT y, int button)
{
    if (!disp) return;
    /* Move to position, then press + release */
    send_input(disp, INPUT_MOUSE_MOVE, x, y, 0);
    send_input(disp, INPUT_MOUSE_BUTTON, x, y, (UINT32)((button & 0xFF) | (1 << 8)));  /* down */
    send_input(disp, INPUT_MOUSE_BUTTON, x, y, (UINT32)(button & 0xFF));                /* up */
}

ASB_API void asb_display_mouse_wheel(AsbDisplay *disp, int delta)
{
    if (!disp) return;
    send_input(disp, INPUT_MOUSE_WHEEL, (UINT32)delta, 0, 0);
}

ASB_API void asb_display_mouse_button(AsbDisplay *disp, UINT x, UINT y,
                                       int button, BOOL down)
{
    if (!disp) return;
    send_input(disp, INPUT_MOUSE_BUTTON, x, y,
               (UINT32)((button & 0xFF) | (down ? (1 << 8) : 0)));
}

/* ---- Keyboard input ---- */

ASB_API void asb_display_key(AsbDisplay *disp, UINT vk, BOOL down)
{
    if (!disp) return;
    send_input(disp, INPUT_KEY, vk, down ? 1 : 0, 0);
}

ASB_API void asb_display_type_text(AsbDisplay *disp, const wchar_t *text)
{
    const wchar_t *p;
    if (!disp || !text) return;

    for (p = text; *p; p++) {
        SHORT vk_info = VkKeyScanW(*p);
        UINT vk = vk_info & 0xFF;
        BOOL shift = (vk_info >> 8) & 1;

        if (vk_info == -1) continue; /* no mapping for this character */

        if (shift) send_input(disp, INPUT_KEY, VK_SHIFT, 1, 0);
        send_input(disp, INPUT_KEY, vk, 1, 0);
        send_input(disp, INPUT_KEY, vk, 0, 0);
        if (shift) send_input(disp, INPUT_KEY, VK_SHIFT, 0, 0);

        Sleep(10); /* small delay between keystrokes */
    }
}
