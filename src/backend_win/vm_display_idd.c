/*
 * vm_display_idd.c -- Host-side IDD frame receiver + D3D11 renderer.
 *
 * Connects to the guest VM over AF_HYPERV sockets:
 *   :0002  Frame channel — receives frames, renders via D3D11 textured quad
 *   :0003  Input channel — forwards keyboard/mouse events to guest
 *   :0004  Audio channel — receives audio from guest, renders via WASAPI
 *
 * Clipboard sync (:0005/:0006) is handled by vm_clipboard.c.
 *
 * Pure C, compiled as C.
 */

#include <winsock2.h>
#include <windows.h>

#define COBJMACROS
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union in SDK headers */
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <avrt.h>
#include <winternl.h>
#include <d3dkmthk.h>
#pragma warning(pop)

#include <stdio.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "asb_core.h"
#include "../core/protocol.h"
#include "../../tools/transport/asb_automation.h"
#include "vm_clipboard.h"
#include "vm_agent.h"
#include "hcs_vm.h"
#include "ui.h"
#include "resource.h"
#include "gpu_broker.h"
#include "agent_overlay.h"
#include "headless_vm.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif


/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* These three are superseded by hcs_service_guid(os_type, port, ...) — kept
   for grep. Windows VMs reach byte-identical GUIDs via the helper; Linux
   VMs reach vsock-template GUIDs. */
static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Input channel service GUID — connects to agent for SendInput injection */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Audio capture channel — connects to guest audio helper (guest→host) */
static const GUID AUDIO_SERVICE_GUID =
    { 0xa5b0cafe, 0x0004, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Audio wire protocol (mirror of tools/agent/appsandbox-audio.c) ---- */

#define AUDIO_HEADER_MAGIC  0x31415341  /* 'ASA1' */

#pragma pack(push, 1)
typedef struct AudioHeader {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;      /* 1=PCM, 3=IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct AudioFrameHeader {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

/* Clipboard reader-apply message (posted by vm_clipboard.c to our wndproc) */
#define WM_CLIP_READER_APPLY (WM_APP + 11)

/* ---- Frame protocol constants ---- */

#define FRAME_MAGIC         0x52465341  /* "ASFR" little-endian */
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MAX_DIRTY_RECTS     64
#define MAX_FRAME_DATA_SIZE (DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT * 4)

/* ---- Window messages ---- */

#define WM_VM_DISPLAY_CLOSED    (WM_APP + 5)
#define WM_IDD_FRAME_READY      (WM_USER + 100)
#define WM_IDD_TITLE_CHANGED    (WM_USER + 110)
#define WM_IDD_FOCUS            (WM_USER + 101)
#define WM_IDD_INPUT_READY      (WM_USER + 102)
#define WM_IDD_CURSOR_CHANGED   (WM_USER + 103)
#define WM_IDD_DISPLAY_SIZE     (WM_USER + 104)
#define WM_IDD_TAKE_CONTROL     (WM_USER + 105)

/* Timer for Present cadence when no frames arrive */
#define IDT_PRESENT     2001
#define PRESENT_MS      16   /* ~60 fps */

/* Debug log window */
#define IDC_LOG_LIST      3001
#define MAX_LOG_LINES     200
#define LOG_WINDOW_W      800
#define LOG_WINDOW_H      300

/* ---- HLSL shaders (inline strings) ---- */

static const char g_vs_hlsl[] =
    "struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    VS_OUT o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char g_ps_hlsl[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    return tex.Sample(samp, uv);\n"
    "}\n";

/* ---- Frame header from wire ---- */

#pragma pack(push, 1)
typedef struct FrameHeader {
    UINT32 magic;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT64 frame_seq;
    UINT32 dirty_rect_count;
} FrameHeader;

/* ---- Cursor header from wire (must match VDD_WIRE_CURSOR_HEADER) ---- */

#define CURSOR_MAGIC        0x52435341  /* "ASCR" little-endian */
#define MAX_CURSOR_SIZE     (256 * 256 * 4 * 2)  /* 2x for MASKED_COLOR double-height */

typedef struct CursorHeader {
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
    UINT32 cursor_type;     /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32 shape_data_size;
} CursorHeader;
#pragma pack(pop)

/* ---- Display context ---- */

typedef struct {
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *srv;
    IDXGIKeyedMutex *mutex;
    ID3D11Fence *fence;
    UINT64 fence_value;
    UINT64 fence_waiting;
    BOOL ready;
    BOOL acquired;
    ULONGLONG screenshot_yielded;
} IddSharedSlot;

struct VmDisplayIdd {
    AsbInputSession *input;
    UINT64       vm_id;
    UINT_PTR     token;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    wchar_t      os_type[32];      /* copy of vm->os_type for picking the service GUID variant */
    wchar_t      gpu_id[512];
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    volatile BOOL open;
    volatile BOOL stop;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    UINT                    swap_flags;
    HANDLE                  present_event;
    BOOL                    present_ready;
    BOOL                    render_pending;
    volatile LONG           render_requested;
    volatile LONG           resize_requested;
    volatile LONG           gpu_refresh_requested;
    CONDITION_VARIABLE      gpu_refresh_done;
    volatile LONG           rendered_size;
    BOOL                    title_muted;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    ID3D11ShaderResourceView *frame_srv;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11SamplerState      *sampler;
    AgentOverlay           *agent_overlay;
    UINT                    pending_agent_command;
    UINT32                  pending_agent_work;
    IddSharedSlot            shared_slots[DISPLAY_GPU_SLOTS];
    int                     shared_front;
    int                     shared_pending;
    UINT64                  shared_session;
    BOOL                    shared_offered;
    BOOL                    shared_rejected;
    BOOL                    shared_ready;
    BOOL                    shared_disabled;
    BOOL                    shared_resize;
    BOOL                    shared_refresh;
    BOOL                    shared_screenshot;
    UINT                    shared_refresh_rate;
    UINT                    shared_width;
    UINT                    shared_height;
    UINT                    display_width;
    UINT                    display_height;
    UINT                    guest_size_mode;
    UINT                    size_mode;
    DisplayGpuPacket        size_request;
    ULONGLONG               size_reconnect_until;
    BOOL                    size_follow_pending;
    BOOL                    size_update_pending;
    SOCKET                  frame_socket;
    UINT                    shared_next;
    HANDLE                  shared_thread;
    HANDLE                  shared_wake;
    HANDLE                  shared_fence_event;
    ID3D11DeviceContext4    *shared_context;
    BOOL                    software_renderer;
    ULONGLONG               render_retry_at;

    /* Frame buffer (CPU-side, updated by recv thread) */
    BYTE          *frame_buf;
    UINT           frame_width;
    UINT           frame_height;
    UINT           frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL  frame_dirty;

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    SRWLOCK        input_lock;
    volatile LONG  keyboard_version;
    volatile LONG  mouse_version;
    volatile BOOL  frame_connected;
    BOOL           relative_mouse;
    volatile BOOL  input_sizing;
    UINT           mouse_buttons;
    BOOL           raw_absolute_valid;
    HANDLE         raw_absolute_device;
    POINT          raw_absolute_position;
    BOOL           mouse_sync_pending;
    UINT32         mouse_sync_id;
    ULONGLONG      mouse_sync_deadline;
    POINT          mouse_sync_origin;
    SOCKET         mouse_sync_socket;
    volatile LONG  agent_control;
    volatile LONG  agent_x, agent_y;
    LONG           agent_waiting, agent_finished;
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */

    /* Keyboard hotkey handling */
    wchar_t        vhdx_path[MAX_PATH]; /* copy of vm->vhdx_path for settings file */
    volatile BOOL  transmit_hotkeys; /* TRUE = capture host hotkeys + send to guest */
    volatile BOOL  input_focused;    /* TRUE while our top-level window is active */
    HHOOK          kbd_hook;          /* WH_KEYBOARD_LL handle, NULL when not installed */
    /* Legacy keys use VK indices; physical keys use scan/E0 or 512 + function VK. */
    BYTE           held_down[768];
    InputPacket    held_keys[768];
    BYTE           hook_routes[768];
    BOOL           input_menu_active;

    /* Guest cursor */
    HCURSOR        guest_cursor;    /* current cursor created from guest bitmap */
    UINT32         cursor_shape_id; /* tracks which shape is current */
    BOOL           cursor_visible;  /* guest cursor visibility */

    /* Debug log window (separate top-level window) */
    HWND           log_hwnd;        /* top-level log window */
    HWND           log_list_hwnd;   /* listbox inside log window */
    HWND           render_hwnd;     /* child window for D3D11 rendering */
    DWORD          window_thread_id;/* owning thread of log/render windows */

    /* Clipboard (extracted to vm_clipboard.c) */
    VmClipboard      clipboard;

    /* Audio playback channel (:0004 — guest→host render) */
    volatile SOCKET  audio_socket;
    HANDLE           audio_recv_thread;
    volatile BOOL    audio_muted;

    /* Threads */
    HANDLE         recv_thread;
    HANDLE         window_thread;
};

/* ---- Forward declarations ---- */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static DWORD WINAPI     idd_window_thread_proc(LPVOID param);
static DWORD WINAPI     idd_recv_thread_proc(LPVOID param);
static void d3d_resize_swap_chain(VmDisplayIdd *d);
static void d3d_render_frame(VmDisplayIdd *d);
static void d3d_recover(VmDisplayIdd *d);
static void d3d_refresh_gpu(VmDisplayIdd *d);
static void idd_request_render(VmDisplayIdd *d, BOOL resize);
static void idd_update_relative_mouse(VmDisplayIdd *d);
static void idd_resume_absolute_mouse(VmDisplayIdd *d, const InputPacket *reply);
static void idd_poll_mouse_position(VmDisplayIdd *d);
static BOOL idd_send_size_request(VmDisplayIdd *d, SOCKET s);
static void idd_update_display_size(VmDisplayIdd *d, BOOL follow);

/* ---- Window class ---- */

static const wchar_t *IDD_DISPLAY_CLASS = L"AppSandboxIddDisplay";
static const wchar_t *IDD_RENDER_CLASS  = L"AppSandboxIddRender";
static const wchar_t *IDD_LOG_CLASS     = L"AppSandboxIddLog";

/* System menu command IDs — must be < 0xF000 and have low 4 bits clear */
#define IDM_AUDIO_MUTE     0x1000
#define IDM_XMIT_HOTKEYS   0x1010
#define IDM_SHOW_LOG       0x1020
#define IDM_MATCH_WINDOW   0x1030
static BOOL g_idd_class_registered;
static WNDPROC g_orig_listbox_proc;
static SRWLOCK g_mouse_capture_lock = SRWLOCK_INIT;
static HWND g_mouse_capture_hwnd;

/* Listbox subclass — handles Ctrl+A / Ctrl+C */
static LRESULT CALLBACK idd_log_listbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (wp == 'C') {
            LRESULT sel_count = SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
            if (sel_count > 0) {
                int *indices = (int *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sel_count * sizeof(int));
                if (indices) {
                    LRESULT i;
                    SIZE_T total = 0;
                    wchar_t *text;
                    SendMessageW(hwnd, LB_GETSELITEMS, (WPARAM)sel_count, (LPARAM)indices);
                    for (i = 0; i < sel_count; i++)
                        total += (SIZE_T)SendMessageW(hwnd, LB_GETTEXTLEN, (WPARAM)indices[i], 0) + 2;
                    total++;
                    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(wchar_t));
                    if (text) {
                        wchar_t *p = text;
                        for (i = 0; i < sel_count; i++) {
                            LRESULT len = SendMessageW(hwnd, LB_GETTEXT, (WPARAM)indices[i], (LPARAM)p);
                            p += len;
                            *p++ = L'\r'; *p++ = L'\n';
                        }
                        if (OpenClipboard(hwnd)) {
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                            if (hMem) {
                                void *dst = GlobalLock(hMem);
                                if (dst) {
                                    memcpy(dst, text, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                                    GlobalUnlock(hMem);
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                } else {
                                    GlobalFree(hMem);
                                }
                            }
                            CloseClipboard();
                        }
                        HeapFree(GetProcessHeap(), 0, text);
                    }
                    HeapFree(GetProcessHeap(), 0, indices);
                }
            }
            return 0;
        }
        if (wp == 'A') {
            LRESULT cnt = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            SendMessageW(hwnd, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, (WORD)(cnt - 1)));
            return 0;
        }
    }
    return CallWindowProcW(g_orig_listbox_proc, hwnd, msg, wp, lp);
}

/* Log window proc — resizes listbox child to fill the window */
static LRESULT CALLBACK idd_log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH s_dark_brush = NULL;

    switch (msg) {
    case WM_SIZE: {
        HWND list = GetDlgItem(hwnd, IDC_LOG_LIST);
        if (list) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(list, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* Match AppSandbox #log-panel: --ctrl-bg #2d2d2d, text #ccc */
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(204, 204, 204));
        SetBkColor(hdc, RGB(45, 45, 45));
        if (!s_dark_brush) s_dark_brush = CreateSolidBrush(RGB(45, 45, 45));
        return (LRESULT)s_dark_brush;
    }
    case WM_CLOSE:
        /* Just hide — the main IDD window owns the lifecycle */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Render child window proc — forwards input + paint to parent for handling */
static LRESULT CALLBACK idd_render_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        /* Validate this window's update region; repaint once for expose/resize
           (rendering is otherwise push-driven by received frames). */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        SendMessageW(GetParent(hwnd), WM_IDD_FRAME_READY, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSELEAVE:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SETCURSOR:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_idd_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    if (g_idd_class_registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_wnd_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_DISPLAY_CLASS;
    RegisterClassExW(&wc);

    /* Render child — D3D11 swap chain targets this window.
       hCursor=NULL so WM_SETCURSOR can set the guest cursor. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_render_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_RENDER_CLASS;
    RegisterClassExW(&wc);

    /* Separate log window — dark background to match AppSandbox main window */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = idd_log_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = IDD_LOG_CLASS;
    RegisterClassExW(&wc);

    g_idd_class_registered = TRUE;
}



/* ---- Debug log panel ---- */

static void idd_log(VmDisplayIdd *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    LRESULT count;
    DWORD_PTR result;
    BOOL from_window_thread;
    LRESULT sent;

    if (!d || !d->log_list_hwnd || d->stop) return;

    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);

    /* Cross-thread SendMessage can deadlock another thread against the window
       thread (e.g. a sender logs while the window thread waits for it). Use a
       bounded SendMessageTimeout off-thread and drop the line on timeout. */
    from_window_thread = (d->window_thread_id == GetCurrentThreadId());

    if (from_window_thread) {
        count = SendMessageW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf);
    } else {
        sent = SendMessageTimeoutW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf,
                                   SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &result);
        if (!sent) return;
        count = (LRESULT)result;
    }

    /* Trim old entries */
    while (count > MAX_LOG_LINES) {
        if (from_window_thread) {
            SendMessageW(d->log_list_hwnd, LB_DELETESTRING, 0, 0);
        } else if (!SendMessageTimeoutW(d->log_list_hwnd, LB_DELETESTRING, 0, 0,
                                        SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &result)) {
            break;
        }
        count--;
    }

    /* Scroll to bottom and force repaint even when not focused (same-thread
       only; off-thread UpdateWindow would block on WM_PAINT the same way) */
    if (from_window_thread) {
        SendMessageW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
        UpdateWindow(d->log_list_hwnd);
    } else {
        SendMessageTimeoutW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &result);
    }
}

/* ---- Send input packet to guest ---- */

static BOOL send_input_locked(VmDisplayIdd *d,UINT32 type,UINT32 p1,UINT32 p2,UINT32 p3)
{
    if (!d->input) return FALSE;
    if (type==ASB_INPUT_USER_GRANT || type==ASB_INPUT_USER_REVOKE || type==ASB_INPUT_USER_CANCEL)
        return !asb_input_control(d->input,type,p1);
    InputPacket packet={INPUT_MAGIC,type,p1,p2,p3};
    DWORD error=asb_input_send(d->input,FALSE,0,&packet);
    return !error || error==ERROR_ACCESS_DENIED;
}

static void send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    AcquireSRWLockExclusive(&d->input_lock);
    send_input_locked(d, type, p1, p2, p3);
    ReleaseSRWLockExclusive(&d->input_lock);
}

static BOOL idd_clip_mouse(VmDisplayIdd *d)
{
    RECT rc;
    if (!GetClientRect(d->render_hwnd, &rc) || IsRectEmpty(&rc)) return FALSE;
    MapWindowPoints(d->render_hwnd, NULL, (POINT *)&rc, 2);
    return ClipCursor(&rc);
}

static void idd_cancel_mouse_sync(VmDisplayIdd *d)
{
    POINT point;
    if (!d->mouse_sync_pending) return;
    d->mouse_sync_pending = FALSE;
    if (GetForegroundWindow() == d->hwnd && GetCursorPos(&point) &&
        WindowFromPoint(point) == d->render_hwnd)
        SetCursor(!d->cursor_visible ? NULL :
                  d->guest_cursor ? d->guest_cursor : LoadCursorW(NULL, IDC_ARROW));
}

static void idd_update_relative_mouse(VmDisplayIdd *d)
{
    POINT pt = {0};
    BOOL active = !d->stop && d->frame_connected && d->agent_control!=ASB_CONTROL_AGENT &&
                   d->mouse_version >= INPUT_MOUSE_VERSION &&
                   d->input_socket != INVALID_SOCKET && d->input_focused &&
                   !d->input_menu_active && !d->input_sizing &&
                   GetForegroundWindow() == d->hwnd && !IsIconic(d->hwnd) &&
                   GetCursorPos(&pt) && WindowFromPoint(pt) == d->render_hwnd;
    BOOL capture = active && !d->cursor_visible;
    BOOL sync_absolute = FALSE;
    if (!active || capture) idd_cancel_mouse_sync(d);
    AcquireSRWLockExclusive(&g_mouse_capture_lock);
    if (capture == d->relative_mouse &&
        (!capture || g_mouse_capture_hwnd == d->hwnd)) {
        ReleaseSRWLockExclusive(&g_mouse_capture_lock);
        return;
    }
    if (capture) {
        RAWINPUTDEVICE mouse = { 0x01, 0x02, 0, d->hwnd };
        d->relative_mouse = FALSE;
        if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
            if (idd_clip_mouse(d)) {
                g_mouse_capture_hwnd = d->hwnd;
                d->relative_mouse = TRUE;
                d->mouse_in = TRUE;
                d->raw_absolute_valid = FALSE;
                SetCursor(NULL);
            } else {
                mouse.dwFlags = RIDEV_REMOVE;
                mouse.hwndTarget = NULL;
                RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            }
        }
    } else {
        if (g_mouse_capture_hwnd == d->hwnd) {
            sync_absolute = d->relative_mouse && active && d->cursor_visible;
            RAWINPUTDEVICE mouse = { 0x01, 0x02, RIDEV_REMOVE, NULL };
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            ClipCursor(NULL);
            g_mouse_capture_hwnd = NULL;
        }
        d->relative_mouse = FALSE;
        d->raw_absolute_valid = FALSE;
    }
    ReleaseSRWLockExclusive(&g_mouse_capture_lock);
    if (sync_absolute && d->frame_width && d->frame_height) {
        if (d->mouse_version >= INPUT_MOUSE_VERSION) {
            if (++d->mouse_sync_id == 0) ++d->mouse_sync_id;
            d->mouse_sync_origin = pt;
            d->mouse_sync_deadline = GetTickCount64() + 250;
            AcquireSRWLockExclusive(&d->input_lock);
            d->mouse_sync_socket = d->input_socket;
            d->mouse_sync_pending = send_input_locked(d, INPUT_MOUSE_POSITION_QUERY,
                                                      d->mouse_sync_id, 0, 0);
            ReleaseSRWLockExclusive(&d->input_lock);
            if (d->mouse_sync_pending) {
                SetCursor(NULL);
                return;
            }
        }
        idd_resume_absolute_mouse(d, NULL);
    }
}

static void idd_flush_mouse_buttons(VmDisplayIdd *d)
{
    for (UINT button = INPUT_BTN_LEFT; button <= INPUT_BTN_MIDDLE; button++) {
        if (d->mouse_buttons & (1u << button))
            send_input(d, INPUT_MOUSE_BUTTON, button, 0, 0);
    }
    d->mouse_buttons = 0;
    if (GetCapture() == d->render_hwnd) ReleaseCapture();
}

/* ==================================================================
 * Per-VM display settings (display_settings.json beside disk.vhdx)
 *
 * Mirrors the vm_state.json pattern in asb_core.c but is owned entirely
 * by the IDD display: the file is created lazily the first time a VM's
 * display opens, so both new and pre-existing VMs get one on demand.
 * ================================================================== */

static void idd_display_settings_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_chars)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_chars, L"%s\\display_settings.json", dir);
}

static void idd_display_settings_save(const wchar_t *vhdx_path, BOOL transmit_hotkeys)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"transmitKeyboardHotkeys\":%d}\n", transmit_hotkeys ? 1 : 0);
    fclose(f);
}

/* Read the persisted setting; if the file is absent, create it with the
   default (off) and return FALSE. Returns the transmit-hotkeys value. */
static BOOL idd_display_settings_load_or_create(const wchar_t *vhdx_path)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    BOOL transmit = FALSE;

    if (!vhdx_path || vhdx_path[0] == L'\0') return FALSE;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);

    if (_wfopen_s(&f, path, L"r") != 0 || !f) {
        /* Lazy creation: file doesn't exist yet (new or pre-existing VM). */
        idd_display_settings_save(vhdx_path, FALSE);
        return FALSE;
    }
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"transmitKeyboardHotkeys\":1"))
            transmit = TRUE;
    }
    fclose(f);
    return transmit;
}

/* ==================================================================
 * Keyboard hotkey capture
 * ================================================================== */

/* Host shortcuts withheld from the guest in Default mode.
   alt_down reflects GetKeyState(VK_MENU) in the window procedure. */
static BOOL idd_is_reserved_hotkey(DWORD vk, BOOL alt_down)
{
    switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SNAPSHOT:   /* PrintScreen */
    case VK_APPS:       /* Menu / context key */
        return TRUE;
    case VK_TAB:
        return alt_down;                                  /* Alt+Tab */
    case VK_ESCAPE:
        /* Alt+Esc / Ctrl+Esc. GetAsyncKeyState reads real-time physical state,
           which is reliable both in the wndproc and inside the low-level hook
           (where the synchronized GetKeyState value may not be updated yet). */
        return alt_down || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    default:
        return FALSE;
    }
}

/* Forward a key event to the guest and track held state so a later focus
   change can release anything still down. Runs on the window thread only. */
static void idd_forward_key(VmDisplayIdd *d, DWORD vk, DWORD scan, BOOL ext, BOOL up)
{
    UINT32 type = INPUT_KEY, index = vk;
    InputPacket pkt;
    AcquireSRWLockExclusive(&d->input_lock);
    if (d->keyboard_version == INPUT_KEYBOARD_VERSION) {
        type = INPUT_KEY_PHYSICAL;
        if (vk == VK_PAUSE || vk == VK_CANCEL) { scan = 0; ext = FALSE; }
        /* Windows flags right Shift and Num Lock as extended without a physical E0 prefix. */
        if (scan == 0x36 || scan == 0x45) ext = FALSE;
        if (vk == VK_PACKET || scan > 255) goto done;
        if (scan == 0 && vk != VK_CANCEL && vk != VK_PAUSE &&
            vk != VK_SNAPSHOT && vk != VK_SLEEP &&
            !(vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2)) goto done;
        index = scan ? scan + (ext ? 256 : 0) : 512 + vk;
    }
    pkt.magic = INPUT_MAGIC;
    pkt.type = type;
    pkt.param1 = vk;
    pkt.param2 = scan;
    pkt.param3 = ext ? INPUT_KEY_EXTENDED : 0;
    if (type == INPUT_KEY_PHYSICAL &&
        index < ARRAYSIZE(d->held_down) && d->held_down[index])
        pkt = d->held_keys[index];
    if (type == INPUT_KEY_PHYSICAL && up && (scan == 0xF1 || scan == 0xF2) &&
        !d->held_down[index]) {
        /* Some Korean keyboard drivers report only the key release. */
        if (!send_input_locked(d, pkt.type, pkt.param1, pkt.param2, pkt.param3)) goto done;
        d->held_down[index] = 1;
        d->held_keys[index] = pkt;
    }
    if (up) pkt.param3 |= INPUT_KEY_UP;
    if (send_input_locked(d, pkt.type, pkt.param1, pkt.param2, pkt.param3) &&
        index < ARRAYSIZE(d->held_down)) {
        d->held_down[index] = !up;
        if (!up) d->held_keys[index] = pkt;
    }
done:
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* Send key-up for every key we believe is still held in the guest, then
   clear tracking. Called when our window loses activation, when Transmit
   mode is turned off, and on teardown. */
static void idd_flush_held_keys(VmDisplayIdd *d)
{
    AcquireSRWLockExclusive(&d->input_lock);
    for (int i = 0; i < ARRAYSIZE(d->held_down); i++) {
        if (d->held_down[i]) {
            const InputPacket *pkt = &d->held_keys[i];
            send_input_locked(d, pkt->type, pkt->param1, pkt->param2,
                              pkt->param3 | INPUT_KEY_UP);
            d->held_down[i] = 0;
        }
    }
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* Thread-local owner: a WH_KEYBOARD_LL callback runs on the thread that
   installed it (our window thread), so this resolves each display's context
   without a global registry, keeping multiple displays independent. */
static __declspec(thread) VmDisplayIdd *t_hook_display;

enum { KEY_ROUTE_HOST = 1, KEY_ROUTE_GUEST, KEY_ROUTE_BOTH };

static BOOL idd_is_modifier(DWORD vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU);
}

static LRESULT CALLBACK idd_ll_keyboard_proc(int code, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = t_hook_display;
    if (code == HC_ACTION && d && !d->stop) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        BOOL ext = (k->flags & LLKHF_EXTENDED) != 0;
        BOOL focused = d->input_focused && GetForegroundWindow() == d->hwnd;
        if (d->keyboard_version != INPUT_KEYBOARD_VERSION) {
            if (focused && d->transmit_hotkeys) {
                idd_forward_key(d, k->vkCode, k->scanCode, ext, up);
                return 1;
            }
        } else {
            DWORD scan = (k->vkCode == VK_PAUSE || k->vkCode == VK_CANCEL)
                         ? 0 : k->scanCode;
            if (scan == 0x36 || scan == 0x45) ext = FALSE;
            if (scan > 255 || k->vkCode > 255) return CallNextHookEx(NULL, code, wp, lp);
            UINT index = scan ? scan + (ext ? 256 : 0) : 512 + k->vkCode;
            BYTE route = d->hook_routes[index];
            BOOL pulse = k->vkCode == VK_PAUSE || k->vkCode == VK_CANCEL;
            if (!focused || d->input_menu_active) {
                d->hook_routes[index] = up || pulse ? 0 : KEY_ROUTE_HOST;
                return CallNextHookEx(NULL, code, wp, lp);
            }
            if (!route) {
                if (up && scan != 0xF1 && scan != 0xF2)
                    return CallNextHookEx(NULL, code, wp, lp);
                if (d->transmit_hotkeys) {
                    route = KEY_ROUTE_GUEST;
                } else if (idd_is_reserved_hotkey(k->vkCode, (k->flags & LLKHF_ALTDOWN) != 0) ||
                           (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                    route = KEY_ROUTE_HOST;
                } else {
                    route = idd_is_modifier(k->vkCode) ? KEY_ROUTE_BOTH : KEY_ROUTE_GUEST;
                }
            }
            /* Releases and repeats follow the key-down destination even if modifiers changed. */
            d->hook_routes[index] = up || pulse ? 0 : route;
            if (route != KEY_ROUTE_HOST)
                idd_forward_key(d, k->vkCode, k->scanCode, ext, up);
            if (route == KEY_ROUTE_GUEST) return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void idd_install_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) return;
    ZeroMemory(d->hook_routes, sizeof(d->hook_routes));
    AcquireSRWLockShared(&d->input_lock);
    if (d->keyboard_version == INPUT_KEYBOARD_VERSION) {
        for (UINT i = 0; i < ARRAYSIZE(d->held_down); i++)
            if (d->held_down[i]) d->hook_routes[i] = KEY_ROUTE_BOTH;
    }
    ReleaseSRWLockShared(&d->input_lock);
    t_hook_display = d;
    d->kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, idd_ll_keyboard_proc,
                                     d->hInstance, 0);
    if (!d->kbd_hook)
        idd_log(d, L"Hotkey hook install failed (err %lu).", GetLastError());
    else
        idd_log(d, L"Hotkey capture enabled.");
}

static void idd_remove_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) {
        UnhookWindowsHookEx(d->kbd_hook);
        d->kbd_hook = NULL;
        idd_log(d, L"Hotkey capture disabled.");
    }
    t_hook_display = NULL;
}

static void idd_update_kbd_hook(VmDisplayIdd *d)
{
    if (d->transmit_hotkeys || d->keyboard_version == INPUT_KEYBOARD_VERSION)
        idd_install_kbd_hook(d);
    else
        idd_remove_kbd_hook(d);
}

/* Compute letterboxed/pillarboxed viewport within client rect */
static void compute_letterbox(UINT client_w, UINT client_h,
                              UINT frame_w, UINT frame_h,
                              float *out_x, float *out_y,
                              float *out_w, float *out_h)
{
    float scale_x, scale_y, scale;
    if (client_w == 0 || client_h == 0 || frame_w == 0 || frame_h == 0) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return;
    }
    scale_x = (float)client_w / (float)frame_w;
    scale_y = (float)client_h / (float)frame_h;
    scale = scale_x < scale_y ? scale_x : scale_y;
    *out_w = (float)frame_w * scale;
    *out_h = (float)frame_h * scale;
    *out_x = ((float)client_w - *out_w) * 0.5f;
    *out_y = ((float)client_h - *out_h) * 0.5f;
}

/* Map window client coordinates to VM framebuffer coordinates */
static void window_to_vm_coords(HWND hwnd, int wx, int wy,
                                 UINT vm_w, UINT vm_h, BOOL normalized,
                                 UINT *vx, UINT *vy)
{
    RECT rc;
    float vp_x, vp_y, vp_w, vp_h;
    float local_x, local_y;

    GetClientRect(hwnd, &rc);
    compute_letterbox((UINT)rc.right, (UINT)rc.bottom, vm_w, vm_h,
                      &vp_x, &vp_y, &vp_w, &vp_h);

    if (vp_w <= 0 || vp_h <= 0) {
        *vx = 0;
        *vy = 0;
        return;
    }

    local_x = ((float)wx - vp_x) / vp_w;
    local_y = ((float)wy - vp_y) / vp_h;

    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    if (local_x > 1) local_x = 1;
    if (local_y > 1) local_y = 1;
    *vx = (UINT)(local_x * (normalized ? INPUT_MOUSE_ABSOLUTE_MAX : vm_w));
    *vy = (UINT)(local_y * (normalized ? INPUT_MOUSE_ABSOLUTE_MAX : vm_h));
    if (!normalized) {
        if (*vx >= vm_w) *vx = vm_w - 1;
        if (*vy >= vm_h) *vy = vm_h - 1;
    }
}

static void send_mouse_move(VmDisplayIdd *d, int x, int y, UINT frame_size)
{
    UINT width = LOWORD(frame_size), height = HIWORD(frame_size);
    UINT vx, vy;
    if (!width || !height) return;
    AcquireSRWLockExclusive(&d->input_lock);
    BOOL normalized = d->mouse_version >= INPUT_MOUSE_NORMALIZED_VERSION;
    window_to_vm_coords(d->render_hwnd, x, y, width, height, normalized, &vx, &vy);
    send_input_locked(d, INPUT_MOUSE_MOVE, vx, vy, normalized ? INPUT_MOUSE_NORMALIZED : 0);
    ReleaseSRWLockExclusive(&d->input_lock);
}

static void idd_resume_absolute_mouse(VmDisplayIdd *d, const InputPacket *reply)
{
    POINT point;
    UINT frame_size = (UINT)InterlockedCompareExchange(&d->rendered_size, 0, 0);
    UINT width = LOWORD(frame_size), height = HIWORD(frame_size);
    if (d->stop || !d->frame_connected || !d->input_focused ||
        d->input_menu_active || d->input_sizing || d->relative_mouse ||
        !d->cursor_visible || !width || !height ||
        GetForegroundWindow() != d->hwnd || IsIconic(d->hwnd) ||
        !GetCursorPos(&point) || WindowFromPoint(point) != d->render_hwnd) {
        idd_cancel_mouse_sync(d);
        return;
    }

    if (reply && reply->param1 < width && reply->param2 < height) {
        RECT rc;
        float x, y, vp_width, vp_height;
        LONG dx = point.x - d->mouse_sync_origin.x;
        LONG dy = point.y - d->mouse_sync_origin.y;
        if (GetClientRect(d->render_hwnd, &rc) && !IsRectEmpty(&rc)) {
            compute_letterbox(rc.right, rc.bottom, width, height,
                              &x, &y, &vp_width, &vp_height);
            POINT target = {
                (LONG)(x + (reply->param1 + 0.5f) * vp_width / width) + dx,
                (LONG)(y + (reply->param2 + 0.5f) * vp_height / height) + dy
            };
            if (target.x < (LONG)x) target.x = (LONG)x;
            if (target.y < (LONG)y) target.y = (LONG)y;
            if (target.x >= (LONG)(x + vp_width)) target.x = (LONG)(x + vp_width) - 1;
            if (target.y >= (LONG)(y + vp_height)) target.y = (LONG)(y + vp_height) - 1;
            if (ClientToScreen(d->render_hwnd, &target)) {
                SetCursorPos(target.x, target.y);
                GetCursorPos(&point);
            }
        }
    }
    d->mouse_sync_pending = FALSE;
    ScreenToClient(d->render_hwnd, &point);
    send_mouse_move(d, point.x, point.y, frame_size);
    SetCursor(d->guest_cursor ? d->guest_cursor : LoadCursorW(NULL, IDC_ARROW));
}

static void idd_poll_mouse_position(VmDisplayIdd *d)
{
    InputPacket position_reply={0},packet;
    BOOL received=FALSE,changed=FALSE;
    ULONGLONG now=GetTickCount64();
    if (d->mouse_sync_pending) idd_update_relative_mouse(d);
    AcquireSRWLockExclusive(&d->input_lock);
    AsbInputStatus status;
    asb_input_status(d->input,&status);
    LONG state=status.flags&ASB_STATUS_AGENT_HAS_CONTROL?ASB_CONTROL_AGENT:
        status.flags&ASB_STATUS_USER_PAUSED?ASB_CONTROL_USER:ASB_CONTROL_AVAILABLE;
    LONG waiting=!!(status.flags&ASB_STATUS_AGENT_WAITING);
    LONG finished=!!(status.flags&ASB_STATUS_AGENT_FINISHED);
    if (waiting!=d->agent_waiting || finished!=d->agent_finished) changed=TRUE;
    d->agent_waiting=waiting;d->agent_finished=finished;
    if (InterlockedExchange(&d->agent_control,state)!=state) {
        changed=TRUE;
        PostMessageW(d->hwnd,WM_IDD_TITLE_CHANGED,0,0);
        PostMessageW(d->hwnd,WM_IDD_INPUT_READY,0,0);
    }
    if (InterlockedExchange(&d->agent_x,(LONG)status.x)!=(LONG)status.x) changed=TRUE;
    if (InterlockedExchange(&d->agent_y,(LONG)status.y)!=(LONG)status.y) changed=TRUE;
    while (asb_input_poll(d->input,&packet)) {
        if (packet.type==INPUT_MOUSE_POSITION_REPLY && d->mouse_sync_pending &&
            d->input_socket==d->mouse_sync_socket && packet.param3==d->mouse_sync_id) {
            position_reply=packet;received=TRUE;
        }
    }
    if (d->pending_agent_command && d->input) {
        asb_input_control(d->input,d->pending_agent_command,d->pending_agent_work);
        d->pending_agent_command=0;
    }
    BOOL disconnected=d->input_socket==INVALID_SOCKET;
    ReleaseSRWLockExclusive(&d->input_lock);
    if (disconnected) idd_cancel_mouse_sync(d);
    else if (received) idd_resume_absolute_mouse(d,&position_reply);
    else if (d->mouse_sync_pending && now>=d->mouse_sync_deadline) idd_resume_absolute_mouse(d,NULL);
    if (changed && d->agent_overlay) {
        UINT size=(UINT)InterlockedCompareExchange(&d->rendered_size,0,0);
        agent_overlay_update(d->agent_overlay,d->agent_control,d->agent_x,d->agent_y,
            LOWORD(size),HIWORD(size),d->agent_waiting,d->agent_finished);
    }
}

/* ---- Reliable recv: read exactly `len` bytes ---- */

static BOOL recv_exact(VmDisplayIdd *d, SOCKET s, void *buf, int len, BOOL idle)
{
    char *p = (char *)buf;
    int remaining = len;
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (remaining > 0) {
        fd_set readable;
        struct timeval timeout = {0, 100000};
        int n;
        if (d->stop) return FALSE;
        if (idle && remaining == len && s == d->frame_socket &&
            !idd_send_size_request(d, s)) return FALSE;
        if ((!idle || remaining != len) && GetTickCount64() >= deadline)
            return FALSE;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        n = select(0, &readable, NULL, NULL, &timeout);
        if (n == SOCKET_ERROR) return FALSE;
        if (n == 0) continue;
        if (idle && remaining == len) deadline = GetTickCount64() + 5000;
        n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_hv_service(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
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

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family   = AF_HYPERV;
    addr.VmId     = *vm_runtime_id;
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

    /* Back to blocking with recv timeout */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

static DWORD WINAPI idd_input_thread_proc(LPVOID param)
{
    VmDisplayIdd *d=param;
    AsbHeadlessTarget target={0};
    target.id=d->vm_id;target.runtime=d->runtime_id;
    wcscpy_s(target.name,ARRAYSIZE(target.name),d->vm_name);
    wcscpy_s(target.os_type,ARRAYSIZE(target.os_type),d->os_type);
    AsbInputSession *input=asb_input_session(&target);
    asb_input_viewer(input,TRUE);
    AcquireSRWLockExclusive(&d->input_lock);
    d->input=input;
    ReleaseSRWLockExclusive(&d->input_lock);
    while (!d->stop) {
        AsbInputStatus status;
        asb_input_status(input,&status);
        AcquireSRWLockExclusive(&d->input_lock);
        BOOL changed=d->input_socket!=status.connection || d->keyboard_version!=(int)status.keyboard_version;
        if (changed) ZeroMemory(d->held_down,sizeof(d->held_down));
        d->input_socket=status.connection;
        d->keyboard_version=status.keyboard_version?status.keyboard_version:1;
        d->mouse_version=status.mouse_version;
        ReleaseSRWLockExclusive(&d->input_lock);
        if (changed) PostMessageW(d->hwnd,WM_IDD_INPUT_READY,0,0);
        Sleep(100);
    }
    AcquireSRWLockExclusive(&d->input_lock);
    d->input=NULL;d->input_socket=INVALID_SOCKET;
    d->keyboard_version=1;d->mouse_version=0;
    ZeroMemory(d->held_down,sizeof(d->held_down));
    ReleaseSRWLockExclusive(&d->input_lock);
    asb_input_viewer(input,FALSE);
    asb_input_release(input);
    PostMessageW(d->hwnd,WM_IDD_INPUT_READY,0,0);
    return 0;
}

/* ==================================================================
 * Audio playback — guest -> host
 * ================================================================== */

static const CLSID AUDIO_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID AUDIO_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID AUDIO_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID AUDIO_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

static DWORD WINAPI audio_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    HRESULT hr;
    BOOL com_ok = FALSE;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pAC = NULL;
    IAudioRenderClient *pRC = NULL;
    WAVEFORMATEX *renderfmt = NULL;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    UINT32 buf_frames = 0;
    UINT32 render_block_align = 0;
    HANDLE render_event = NULL;
    HANDLE audio_task = NULL;
    DWORD audio_task_index = 0;
    AudioHeader hdr;
    BOOL stream_started = FALSE;
    BOOL session_logged = FALSE;
    int  header_misses = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        com_ok = TRUE;
    audio_task = AvSetMmThreadCharacteristicsW(L"Audio", &audio_task_index);

    /* Outer retry loop — keep trying to connect to the guest capture helper */
    while (!d->stop) {
        GUID svc; hcs_service_guid(d->os_type, 4, &svc);
        SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
        if (s == INVALID_SOCKET) {
            int wait;
            for (wait = 0; wait < 2000 && !d->stop; wait += 200)
                Sleep(200);
            continue;
        }

        /* Blocking recv for the session */
        {
            DWORD no_timeout = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
        }
        EnterCriticalSection(&d->frame_cs);
        if (d->stop) {
            LeaveCriticalSection(&d->frame_cs);
            closesocket(s);
            break;
        }
        d->audio_socket = s;
        LeaveCriticalSection(&d->frame_cs);

        /* Read one-shot AudioHeader.
           The guest closes without sending a header when the VAD endpoint
           isn't ready yet (e.g. before user login / audio stack init).
           Log only the first miss per run so we don't spam. */
        if (!recv_exact(d, s, &hdr, sizeof(hdr), FALSE) || hdr.magic != AUDIO_HEADER_MAGIC) {
            if (header_misses == 0)
                idd_log(d, L"Audio header bad/absent - guest not ready, retrying quietly.");
            header_misses++;
            goto session_cleanup;
        }

        idd_log(d, L"Audio connected (GUID :0004).");
        session_logged = TRUE;
        header_misses = 0;

        idd_log(d, L"Audio header: %lu Hz, %u ch, %u bits, tag=%u.",
                 hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);

        /* Set up WASAPI render on the default endpoint */
        hr = CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL,
                               CLSCTX_ALL, &AUDIO_IID_IMMDeviceEnumerator,
                               (void **)&pEnum);
        if (FAILED(hr)) { idd_log(d, L"Audio: CoCreateInstance failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDev);
        if (FAILED(hr)) { idd_log(d, L"Audio: GetDefaultAudioEndpoint failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDevice_Activate(pDev, &AUDIO_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC);
        if (FAILED(hr)) { idd_log(d, L"Audio: Activate failed 0x%08lX.", hr); goto session_cleanup; }

        /* Build the source format exactly matching what the guest is sending.
           We deliberately do NOT call GetMixFormat: we never use the host's
           mix format (Initialize runs with AUTOCONVERTPCM, so WASAPI converts
           our renderfmt to whatever the endpoint needs). On some endpoints
           (seen on AMD HD Audio) GetMixFormat returns
           AUDCLNT_E_UNSUPPORTED_FORMAT for the configured default format,
           which would needlessly kill the whole audio path. */
        renderfmt = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!renderfmt) goto session_cleanup;
        ZeroMemory(renderfmt, sizeof(WAVEFORMATEX));
        renderfmt->wFormatTag      = hdr.format_tag;
        renderfmt->nChannels       = hdr.channels;
        renderfmt->nSamplesPerSec  = hdr.sample_rate;
        renderfmt->wBitsPerSample  = hdr.bits_per_sample;
        renderfmt->nBlockAlign     = hdr.block_align ? hdr.block_align
                                     : (WORD)((hdr.channels * hdr.bits_per_sample) / 8);
        renderfmt->nAvgBytesPerSec = renderfmt->nSamplesPerSec * renderfmt->nBlockAlign;
        renderfmt->cbSize          = 0;
        render_block_align         = renderfmt->nBlockAlign;

        hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      400000, 0, renderfmt, NULL);
        if (FAILED(hr)) {
            idd_log(d, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr);
            goto session_cleanup;
        }

        hr = IAudioClient_GetBufferSize(pAC, &buf_frames);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioRenderClient, (void **)&pRC);
        if (FAILED(hr)) goto session_cleanup;

        render_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!render_event) goto session_cleanup;
        hr = IAudioClient_SetEventHandle(pAC, render_event);
        if (FAILED(hr)) goto session_cleanup;

        idd_log(d, L"Audio render buffer: %lu frames.", buf_frames);

        /* Receive loop */
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 frame_offset = 0;

            if (!recv_exact(d, s, &fh, sizeof(fh), TRUE)) break;
            if (fh.bytes == 0 || fh.bytes > 4 * 1024 * 1024 ||
                !render_block_align || fh.bytes % render_block_align) break;

            if (fh.bytes > scratch_cap) {
                BYTE *nb = scratch
                    ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, fh.bytes)
                    : (BYTE *)HeapAlloc(GetProcessHeap(), 0, fh.bytes);
                if (!nb) break;
                scratch = nb;
                scratch_cap = fh.bytes;
            }
            if (!recv_exact(d, s, scratch, (int)fh.bytes, FALSE)) break;

            /* Host-side mute: keep draining the socket but don't push to WASAPI */
            if (d->audio_muted) continue;

            frames_in_payload = fh.bytes / render_block_align;
            if (frames_in_payload == 0) continue;

            while (frame_offset < frames_in_payload && !d->stop && !d->audio_muted) {
                UINT32 padding, free_frames, frames_to_write;
                BYTE *dst;
                hr = IAudioClient_GetCurrentPadding(pAC, &padding);
                if (FAILED(hr) || padding > buf_frames) goto session_cleanup;
                if (padding == 0 && buf_frames > 1) {
                    UINT32 lead_frames = buf_frames / 2;
                    hr = IAudioRenderClient_GetBuffer(pRC, lead_frames, &dst);
                    if (FAILED(hr)) goto session_cleanup;
                    hr = IAudioRenderClient_ReleaseBuffer(pRC, lead_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                    if (FAILED(hr)) goto session_cleanup;
                    padding = lead_frames;
                }
                free_frames = buf_frames - padding;
                if (!free_frames) {
                    DWORD wait = WaitForSingleObject(render_event, 100);
                    if (wait == WAIT_FAILED) goto session_cleanup;
                    continue;
                }
                frames_to_write = frames_in_payload - frame_offset;
                if (frames_to_write > free_frames) frames_to_write = free_frames;
                hr = IAudioRenderClient_GetBuffer(pRC, frames_to_write, &dst);
                if (FAILED(hr)) goto session_cleanup;
                memcpy(dst, scratch + (size_t)frame_offset * render_block_align,
                    (size_t)frames_to_write * render_block_align);
                hr = IAudioRenderClient_ReleaseBuffer(pRC, frames_to_write, 0);
                if (FAILED(hr)) goto session_cleanup;
                frame_offset += frames_to_write;
                if (!stream_started) {
                    ResetEvent(render_event);
                    hr = IAudioClient_Start(pAC);
                    if (FAILED(hr)) goto session_cleanup;
                    stream_started = TRUE;
                }
            }
        }

session_cleanup:
        if (stream_started && pAC) {
            IAudioClient_Stop(pAC);
            stream_started = FALSE;
        }
        if (pRC)    { IAudioRenderClient_Release(pRC);    pRC = NULL; }
        if (pAC)    { IAudioClient_Release(pAC);          pAC = NULL; }
        if (render_event) { CloseHandle(render_event); render_event = NULL; }
        if (pDev)   { IMMDevice_Release(pDev);            pDev = NULL; }
        if (pEnum)  { IMMDeviceEnumerator_Release(pEnum); pEnum = NULL; }
        if (renderfmt) { CoTaskMemFree(renderfmt); renderfmt = NULL; }

        EnterCriticalSection(&d->frame_cs);
        d->audio_socket = INVALID_SOCKET;
        LeaveCriticalSection(&d->frame_cs);
        closesocket(s);
        if (session_logged) {
            idd_log(d, L"Audio session ended.");
            session_logged = FALSE;
        }

        if (d->stop) break;
        /* Back off when the guest keeps rejecting us (VAD not ready) */
        {
            int wait, delay = header_misses > 3 ? 5000 : 500;
            for (wait = 0; wait < delay && !d->stop; wait += 100)
                Sleep(100);
        }
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (audio_task) AvRevertMmThreadCharacteristics(audio_task);
    if (com_ok)  CoUninitialize();
    return 0;
}


/* ==================================================================
 * D3D11 initialization and teardown
 * ================================================================== */

static BOOL d3d_compile_shader(const char *hlsl, const char *entry,
                               const char *target, ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), NULL, NULL, NULL,
                            entry, target, 0, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            ui_log(L"Shader compile error: %S",
                   (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        return FALSE;
    }
    if (errors) errors->lpVtbl->Release(errors);
    return TRUE;
}

typedef struct {
    HANDLE resource;
    GUID vm_id;
    UINT64 process_id;
    UINT64 guest_handle;
    ACCESS_MASK access;
    UINT flags;
} IddDuplicateResource;

C_ASSERT(sizeof(IddDuplicateResource) == 48);
C_ASSERT(sizeof(DisplayGpuPacket) == 68);

static HRESULT idd_shared_unlock(VmDisplayIdd *d, IddSharedSlot *slot)
{
    HRESULT hr = S_OK;
    if (slot->acquired) {
        if (slot->mutex) hr = slot->mutex->lpVtbl->ReleaseSync(slot->mutex, 0);
        else if (slot->fence && d->shared_context) {
            hr = ID3D11DeviceContext4_Signal(d->shared_context, slot->fence, slot->fence_value + 1);
            slot->fence_value += 2;
            d->ctx->lpVtbl->Flush(d->ctx);
        } else hr = E_FAIL;
        slot->acquired = FALSE;
    }
    return hr;
}

static void idd_shared_release(VmDisplayIdd *d)
{
    UINT i;
    for (i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
        IddSharedSlot *slot = &d->shared_slots[i];
        idd_shared_unlock(d, slot);
        if (slot->srv) slot->srv->lpVtbl->Release(slot->srv);
        if (slot->mutex) slot->mutex->lpVtbl->Release(slot->mutex);
        if (slot->fence) ID3D11Fence_Release(slot->fence);
        if (slot->texture) slot->texture->lpVtbl->Release(slot->texture);
        ZeroMemory(slot, sizeof(*slot));
    }
    d->shared_front = -1;
    d->shared_pending = -1;
    d->shared_ready = FALSE;
    d->shared_resize = FALSE;
    d->shared_refresh = FALSE;
    d->shared_screenshot = FALSE;
    d->shared_refresh_rate = 0;
    ZeroMemory(&d->size_request, sizeof(d->size_request));
    d->shared_next = 0;
    if (d->shared_context) ID3D11DeviceContext4_Release(d->shared_context);
    d->shared_context = NULL;
    if (d->shared_wake) SetEvent(d->shared_wake);
}

static void idd_shared_fail(VmDisplayIdd *d)
{
    d->shared_disabled = TRUE;
    idd_shared_release(d);
    if (d->frame_socket != INVALID_SOCKET) shutdown(d->frame_socket, SD_BOTH);
}

static HRESULT idd_shared_acquire(VmDisplayIdd *d, DWORD timeout)
{
    IDXGIKeyedMutex *mutex;
    ID3D11Fence *fence;
    UINT index;
    UINT64 session, value;
    HRESULT hr;
    EnterCriticalSection(&d->frame_cs);
    if (d->stop || !d->shared_ready || d->shared_slots[d->shared_next].acquired ||
        d->shared_slots[d->shared_next].screenshot_yielded) {
        LeaveCriticalSection(&d->frame_cs);
        return S_FALSE;
    }
    index = d->shared_next;
    session = d->shared_session;
    mutex = d->shared_slots[index].mutex;
    fence = d->shared_slots[index].fence;
    value = d->shared_slots[index].fence_value;
    hr = S_OK;
    if (mutex) mutex->lpVtbl->AddRef(mutex);
    else if (fence) {
        ID3D11Fence_AddRef(fence);
        if (d->shared_slots[index].fence_waiting != value) {
            hr = ID3D11Fence_SetEventOnCompletion(fence, value, d->shared_fence_event);
            if (SUCCEEDED(hr)) d->shared_slots[index].fence_waiting = value;
        }
    } else hr = E_FAIL;
    LeaveCriticalSection(&d->frame_cs);

    if (mutex) hr = mutex->lpVtbl->AcquireSync(mutex, 1, timeout);
    else if (fence && SUCCEEDED(hr)) {
        UINT64 completed = ID3D11Fence_GetCompletedValue(fence);
        if (completed < value && timeout) {
            WaitForSingleObject(d->shared_fence_event, timeout);
            completed = ID3D11Fence_GetCompletedValue(fence);
        }
        hr = completed == UINT64_MAX ? DXGI_ERROR_DEVICE_REMOVED :
             completed >= value ? S_OK : WAIT_TIMEOUT;
    }

    EnterCriticalSection(&d->frame_cs);
    if (d->stop || !d->shared_ready || d->shared_session != session ||
        d->shared_slots[index].mutex != mutex || d->shared_slots[index].fence != fence) {
        if (hr == S_OK && mutex) mutex->lpVtbl->ReleaseSync(mutex, 0);
        LeaveCriticalSection(&d->frame_cs);
        if (mutex) mutex->lpVtbl->Release(mutex);
        if (fence) ID3D11Fence_Release(fence);
        return S_FALSE;
    }
    if (hr == WAIT_TIMEOUT && d->device)
        hr = FAILED(d->device->lpVtbl->GetDeviceRemovedReason(d->device))
            ? DXGI_ERROR_DEVICE_REMOVED : hr;
    if (hr == S_OK && fence)
        hr = ID3D11DeviceContext4_Wait(d->shared_context, fence, value);
    if (hr == S_OK) {
        BOOL first_frame = d->shared_front < 0 && d->shared_pending < 0;
        d->shared_slots[index].acquired = TRUE;
        if (d->shared_pending >= 0) {
            IddSharedSlot *old = &d->shared_slots[d->shared_pending];
            hr = idd_shared_unlock(d, old);
            d->shared_pending = -1;
        }
        if (hr == S_OK) {
            d->shared_pending = (int)index;
            if (first_frame) {
                BOOL size_changed = d->display_width != d->shared_width ||
                    d->display_height != d->shared_height;
                if (d->frame_buf) HeapFree(GetProcessHeap(), 0, d->frame_buf);
                d->frame_buf = NULL;
                if (d->frame_srv) d->frame_srv->lpVtbl->Release(d->frame_srv);
                d->frame_srv = NULL;
                if (d->frame_tex) d->frame_tex->lpVtbl->Release(d->frame_tex);
                d->frame_tex = NULL;
                d->frame_width = d->shared_width;
                d->frame_height = d->shared_height;
                d->frame_stride = d->frame_width * 4;
                d->display_width = d->shared_width;
                d->display_height = d->shared_height;
                PostMessageW(d->hwnd, WM_IDD_DISPLAY_SIZE, size_changed, 0);
            }
            d->shared_next = (index + 1) % DISPLAY_GPU_SLOTS;
            d->frame_dirty = TRUE;
        }
    }
    if (hr != S_OK && hr != WAIT_TIMEOUT) idd_shared_fail(d);
    LeaveCriticalSection(&d->frame_cs);
    if (mutex) mutex->lpVtbl->Release(mutex);
    if (fence) ID3D11Fence_Release(fence);
    return hr;
}

static DWORD WINAPI idd_shared_thread_proc(LPVOID parameter)
{
    VmDisplayIdd *d = (VmDisplayIdd *)parameter;
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Playback", &task_index);
    while (!d->stop) {
        HANDLE events[2] = { d->shared_wake, NULL };
        DWORD count = 1, wait, timeout;
        UINT i;
        BOOL can_acquire;
        BOOL screenshot_pending = FALSE;
        BOOL render = InterlockedExchange(&d->render_requested, 0) != 0;
        if (InterlockedCompareExchange(&d->gpu_refresh_requested, 0, 0)) {
            d3d_refresh_gpu(d);
            render = TRUE;
        }
        if (InterlockedExchange(&d->resize_requested, 0)) {
            EnterCriticalSection(&d->frame_cs);
            d3d_resize_swap_chain(d);
            LeaveCriticalSection(&d->frame_cs);
            render = TRUE;
        }
        EnterCriticalSection(&d->frame_cs);
        for (i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
            IddSharedSlot *slot = &d->shared_slots[i];
            if (!slot->screenshot_yielded) continue;
            HRESULT hr = slot->mutex->lpVtbl->AcquireSync(slot->mutex, 1, 0);
            if (hr == S_OK) {
                slot->screenshot_yielded = 0;
                slot->acquired = TRUE;
                if ((int)i != d->shared_front && (int)i != d->shared_pending)
                    hr = idd_shared_unlock(d, slot);
                render = TRUE;
            } else if (hr == (HRESULT)WAIT_TIMEOUT &&
                GetTickCount64() - slot->screenshot_yielded < 2000) {
                screenshot_pending = TRUE;
                continue;
            }
            if (hr != S_OK) { idd_shared_fail(d); break; }
        }
        render = render || (d->frame_dirty && d->rtv != NULL);
        LeaveCriticalSection(&d->frame_cs);
        timeout = render || d->render_pending ? 0 : 16;
        for (i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
            if (idd_shared_acquire(d, timeout) != S_OK) break;
            render = TRUE;
            timeout = 0;
        }
        if (render || d->render_pending) d3d_render_frame(d);
        if (d->stop) break;
        EnterCriticalSection(&d->frame_cs);
        can_acquire = d->shared_ready && !d->shared_slots[d->shared_next].acquired &&
            !d->shared_slots[d->shared_next].screenshot_yielded;
        LeaveCriticalSection(&d->frame_cs);
        if (can_acquire && !d->render_pending) continue;
        if (d->render_pending && d->present_event) {
            events[count++] = d->present_event;
        }
        wait = WaitForMultipleObjects(count, events, FALSE, screenshot_pending ? 1 : 1000);
        if (count == 2 && wait == WAIT_OBJECT_0 + 1) d->present_ready = TRUE;
        else if (wait == WAIT_TIMEOUT) InterlockedExchange(&d->render_requested, 1);
        else if (wait == WAIT_FAILED) d3d_recover(d);
    }
    if (task) AvRevertMmThreadCharacteristics(task);
    return 0;
}

static void idd_request_render(VmDisplayIdd *d, BOOL resize)
{
    if (!d || d->stop) return;
    if (!d->shared_thread && d->shared_disabled) {
        if (resize) d3d_resize_swap_chain(d);
        d3d_render_frame(d);
        return;
    }
    if (resize) InterlockedExchange(&d->resize_requested, 1);
    InterlockedExchange(&d->render_requested, 1);
    if (d->shared_wake) SetEvent(d->shared_wake);
}

static BOOL idd_send_gpu(SOCKET s, const DisplayGpuPacket *packet)
{
    const char *bytes = (const char *)packet;
    int remaining = sizeof(*packet);
    while (remaining) {
        int sent = send(s, bytes, remaining, 0);
        if (sent <= 0) return FALSE;
        bytes += sent;
        remaining -= sent;
    }
    return TRUE;
}

static BOOL idd_shared_start_linux(VmDisplayIdd *d, SOCKET socket,
                                    const DisplayGpuPacket *caps, AsbGpuBroker **broker)
{
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    ID3D11Device5 *device = NULL;
    ID3D11DeviceContext4 *context = NULL;
    DXGI_ADAPTER_DESC desc = {0};
    HRESULT hr = E_INVALIDARG;
    EnterCriticalSection(&d->frame_cs);
    if (!d->stop && d->device && d->ctx && d->shared_thread && d->shared_fence_event &&
        !d->shared_disabled && caps->width && caps->height &&
        caps->width <= DISPLAY_MAX_WIDTH && caps->height <= DISPLAY_MAX_HEIGHT &&
        caps->format == DXGI_FORMAT_B8G8R8A8_UNORM && !caps->status) {
        hr = d->device->lpVtbl->QueryInterface(d->device, &IID_ID3D11Device5, (void **)&device);
        if (SUCCEEDED(hr)) hr = d->ctx->lpVtbl->QueryInterface(d->ctx,
            &IID_ID3D11DeviceContext4, (void **)&context);
        if (SUCCEEDED(hr)) hr = d->device->lpVtbl->QueryInterface(d->device, &IID_IDXGIDevice, (void **)&dxgi);
    }
    LeaveCriticalSection(&d->frame_cs);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetDesc(adapter, &desc);
    if (SUCCEEDED(hr)) {
        *broker = asb_gpu_broker_open(&d->runtime_id, desc.AdapterLuid);
        if (!*broker) hr = E_ACCESSDENIED;
    }
    EnterCriticalSection(&d->frame_cs);
    if (d->stop || d->frame_socket != socket || d->shared_disabled) hr = E_ABORT;
    if (SUCCEEDED(hr)) {
        d->shared_context = context;
        context = NULL;
        d->shared_width = caps->width;
        d->shared_height = caps->height;
        d->shared_resize = (caps->reserved & DISPLAY_GPU_CAP_RESIZE) != 0;
        d->shared_refresh = (caps->reserved & DISPLAY_GPU_CAP_REFRESH) != 0;
        d->shared_refresh_rate = d->shared_refresh ? (UINT)caps->handle : 60;
        d->guest_size_mode = caps->slot == DISPLAY_SIZE_WINDOW ? DISPLAY_SIZE_WINDOW : DISPLAY_SIZE_GUEST;
    }
    LeaveCriticalSection(&d->frame_cs);
    DisplayGpuPacket reply = *caps;
    reply.type = DISPLAY_GPU_OFFER;
    reply.status = (UINT32)hr;
    reply.handle = 0;
    if (FAILED(hr)) {
        asb_gpu_broker_close(*broker);
        *broker = NULL;
        ui_log(L"IDD: Linux GPU import unavailable (0x%08X); using socket frames.", hr);
    }
    if (context) ID3D11DeviceContext4_Release(context);
    if (device) ID3D11Device5_Release(device);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi) IDXGIDevice_Release(dxgi);
    return idd_send_gpu(socket, &reply);
}

static BOOL idd_shared_import_linux(VmDisplayIdd *d, SOCKET socket,
                                    const DisplayGpuPacket *offer, AsbGpuBroker *broker)
{
    DisplayGpuPacket reply = *offer;
    ID3D11Device5 *device = NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Fence *fence = NULL;
    HANDLE handle = NULL;
    HRESULT hr = E_INVALIDARG;
    UINT index = offer->slot % DISPLAY_GPU_SLOTS;
    BOOL is_fence = offer->slot >= DISPLAY_GPU_SLOTS;
    reply.type = DISPLAY_GPU_READY;
    reply.handle = 0;
    EnterCriticalSection(&d->frame_cs);
    if (!d->stop && !d->shared_disabled && d->device && d->shared_context && !d->shared_ready &&
        offer->slot < DISPLAY_GPU_FENCE_SLOTS && offer->width == d->shared_width &&
        offer->height == d->shared_height && offer->format == DXGI_FORMAT_B8G8R8A8_UNORM &&
        !(is_fence ? d->shared_slots[index].fence != NULL : d->shared_slots[index].texture != NULL))
        hr = d->device->lpVtbl->QueryInterface(d->device, &IID_ID3D11Device5, (void **)&device);
    LeaveCriticalSection(&d->frame_cs);
    if (SUCCEEDED(hr)) hr = asb_gpu_broker_import(broker, offer->handle, is_fence,
        offer->width, offer->height, &handle);
    if (SUCCEEDED(hr)) {
        if (is_fence) hr = ID3D11Device5_OpenSharedFence(device, handle, &IID_ID3D11Fence, (void **)&fence);
        else hr = ID3D11Device5_OpenSharedResource1(device, handle, &IID_ID3D11Texture2D, (void **)&texture);
    }
    if (SUCCEEDED(hr) && texture) {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(texture, &desc);
        if (desc.Width != offer->width || desc.Height != offer->height || (UINT)desc.Format != offer->format ||
            desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags || desc.SampleDesc.Count != 1)
            hr = E_INVALIDARG;
        else hr = ID3D11Device5_CreateShaderResourceView(device, (ID3D11Resource *)texture, NULL, &srv);
    }
    EnterCriticalSection(&d->frame_cs);
    if (d->stop || d->frame_socket != socket || d->shared_disabled || !d->shared_context) hr = E_ABORT;
    if (SUCCEEDED(hr)) {
        IddSharedSlot *slot = &d->shared_slots[index];
        if (is_fence) { slot->fence = fence; slot->fence_value = 1; fence = NULL; }
        else { slot->texture = texture; slot->srv = srv; texture = NULL; srv = NULL; }
        BOOL ready = TRUE;
        for (UINT i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
            d->shared_slots[i].ready = d->shared_slots[i].texture && d->shared_slots[i].fence;
            ready = ready && d->shared_slots[i].ready;
        }
        d->shared_ready = ready;
        if (ready) SetEvent(d->shared_wake);
    }
    LeaveCriticalSection(&d->frame_cs);
    if (handle) CloseHandle(handle);
    if (texture) ID3D11Texture2D_Release(texture);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (fence) ID3D11Fence_Release(fence);
    if (device) ID3D11Device5_Release(device);
    reply.status = (UINT32)hr;
    BOOL sent = idd_send_gpu(socket, &reply);
    return sent && SUCCEEDED(hr);
}

static BOOL idd_send_size_request(VmDisplayIdd *d, SOCKET s)
{
    DisplayGpuPacket packet = {0};
    EnterCriticalSection(&d->frame_cs);
    if (!d->input_sizing && d->shared_ready && d->shared_resize && !d->shared_disabled &&
        d->frame_socket == s && d->size_request.type &&
        (d->shared_front >= 0 || d->shared_pending >= 0)) {
        packet = d->size_request;
        packet.session = d->shared_session;
        ZeroMemory(&d->size_request, sizeof(d->size_request));
        d->size_reconnect_until = GetTickCount64() + 10000;
    }
    LeaveCriticalSection(&d->frame_cs);
    return !packet.type || idd_send_gpu(s, &packet);
}

static UINT idd_monitor_refresh(HWND hwnd)
{
    MONITORINFOEXW monitor = {0};
    DEVMODEW settings = {0};
    UINT refresh = 60;
    monitor.cbSize = sizeof(monitor);
    settings.dmSize = sizeof(settings);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&monitor))
        return refresh;
    if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &settings))
        refresh = settings.dmDisplayFrequency;
    return refresh >= DISPLAY_MIN_REFRESH ? min(refresh, DISPLAY_MAX_REFRESH) : 60;
}

static void idd_update_display_size(VmDisplayIdd *d, BOOL follow)
{
    RECT rc;
    UINT width, height, mode, refresh;
    BOOL active, resize_window = FALSE;
    if (!d || !d->hwnd) return;
    d->size_update_pending = TRUE;
    if (follow) d->size_follow_pending = TRUE;
    if (!TryEnterCriticalSection(&d->frame_cs)) return;
    active = d->shared_ready && d->shared_resize && !d->shared_disabled &&
        (d->shared_front >= 0 || d->shared_pending >= 0);
    mode = d->size_mode;
    if ((mode == DISPLAY_SIZE_WINDOW && !active) ||
        d->input_sizing || IsIconic(d->hwnd)) {
        if (mode == DISPLAY_SIZE_WINDOW && !active) d->size_update_pending = FALSE;
        LeaveCriticalSection(&d->frame_cs);
        return;
    }
    d->size_update_pending = FALSE;
    width = d->frame_width;
    height = d->frame_height;
    refresh = 60;
    if (mode == DISPLAY_SIZE_WINDOW) {
        if (d->shared_refresh) refresh = idd_monitor_refresh(d->hwnd);
        GetClientRect(d->hwnd, &rc);
        width = max(DISPLAY_MIN_WIDTH, min(DISPLAY_MAX_WIDTH, (UINT)rc.right));
        height = max(DISPLAY_MIN_HEIGHT, min(DISPLAY_MAX_HEIGHT, (UINT)rc.bottom));
        resize_window = width != (UINT)rc.right || height != (UINT)rc.bottom;
    } else if (d->size_follow_pending) {
        resize_window = TRUE;
    }
    d->size_follow_pending = FALSE;
    ZeroMemory(&d->size_request, sizeof(d->size_request));
    if (active && ((mode == DISPLAY_SIZE_WINDOW && d->shared_refresh && refresh != d->shared_refresh_rate) ||
        mode != d->guest_size_mode || (mode == DISPLAY_SIZE_WINDOW &&
        (width != d->shared_width || height != d->shared_height)))) {
        DisplayGpuPacket *packet = &d->size_request;
        ZeroMemory(packet, sizeof(*packet));
        packet->magic = DISPLAY_GPU_MAGIC;
        packet->type = DISPLAY_GPU_RESIZE;
        packet->version = DISPLAY_GPU_VERSION;
        packet->width = width;
        packet->height = height;
        packet->slot = mode;
        packet->handle = d->shared_refresh ? refresh : 0;
    }
    LeaveCriticalSection(&d->frame_cs);
    if (resize_window) {
        rc.left = rc.top = 0;
        rc.right = (LONG)width;
        rc.bottom = (LONG)height;
        AdjustWindowRectExForDpi(&rc, (DWORD)GetWindowLongW(d->hwnd, GWL_STYLE),
            FALSE, (DWORD)GetWindowLongW(d->hwnd, GWL_EXSTYLE), GetDpiForWindow(d->hwnd));
        if (IsZoomed(d->hwnd)) ShowWindow(d->hwnd, SW_RESTORE);
        SetWindowPos(d->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}


static BOOL idd_shared_offer(VmDisplayIdd *d, SOCKET s, const DisplayGpuPacket *caps, UINT index)
{
    typedef LONG (WINAPI *DuplicateFn)(IddDuplicateResource *);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    DuplicateFn duplicate = gdi ? (DuplicateFn)GetProcAddress(gdi, "D3DKMTDuplicateHandle") : NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    IDXGIKeyedMutex *mutex = NULL;
    IDXGIResource1 *resource = NULL;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC adapter_desc;
    D3D11_TEXTURE2D_DESC desc = {0};
    IddDuplicateResource args = {0};
    DisplayGpuPacket offer = {0};
    HANDLE handle = NULL;
    HRESULT hr = E_NOTIMPL;
    LONG status;

    EnterCriticalSection(&d->frame_cs);
    offer.magic = DISPLAY_GPU_MAGIC;
    offer.type = DISPLAY_GPU_OFFER;
    offer.version = DISPLAY_GPU_VERSION;
    offer.session = caps->session;
    offer.process_id = caps->process_id;
    offer.slot = index;
    if (caps->status != S_OK) {
        hr = (HRESULT)caps->status;
        goto done;
    }
    if (d->shared_disabled || !d->device || !d->shared_thread || !duplicate || !caps->process_id ||
        !caps->width || !caps->height || caps->width > DISPLAY_MAX_WIDTH ||
        caps->height > DISPLAY_MAX_HEIGHT ||
        caps->format != DXGI_FORMAT_B8G8R8A8_UNORM) goto done;

    hr = d->device->lpVtbl->QueryInterface(d->device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (FAILED(hr)) goto done;
    hr = dxgi_device->lpVtbl->GetAdapter(dxgi_device, &adapter);
    if (FAILED(hr)) goto done;
    hr = adapter->lpVtbl->GetDesc(adapter, &adapter_desc);
    if (FAILED(hr)) goto done;
    if (adapter_desc.VendorId != caps->vendor_id || adapter_desc.DeviceId != caps->device_id) {
        hr = DXGI_ERROR_UNSUPPORTED;
        goto done;
    }

    desc.Width = caps->width;
    desc.Height = caps->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = (DXGI_FORMAT)caps->format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    hr = d->device->lpVtbl->CreateTexture2D(d->device, &desc, NULL, &texture);
    if (FAILED(hr)) goto done;
    hr = d->device->lpVtbl->CreateShaderResourceView(d->device, (ID3D11Resource *)texture, NULL, &srv);
    if (FAILED(hr)) goto done;
    hr = texture->lpVtbl->QueryInterface(texture, &IID_IDXGIKeyedMutex, (void **)&mutex);
    if (FAILED(hr)) goto done;
    hr = texture->lpVtbl->QueryInterface(texture, &IID_IDXGIResource1, (void **)&resource);
    if (FAILED(hr)) goto done;
    hr = resource->lpVtbl->CreateSharedHandle(resource, NULL,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle);
    if (FAILED(hr)) goto done;

    args.resource = handle;
    args.vm_id = d->runtime_id;
    args.process_id = caps->process_id;
    args.flags = 1;
    status = duplicate(&args);
    hr = status < 0 ? HRESULT_FROM_NT(status) : (args.guest_handle ? S_OK : E_FAIL);
    if (FAILED(hr)) goto done;
    offer.handle = args.guest_handle;
    offer.width = caps->width;
    offer.height = caps->height;
    offer.format = caps->format;
    d->shared_slots[index].texture = texture;
    d->shared_slots[index].srv = srv;
    d->shared_slots[index].mutex = mutex;
    texture = NULL;
    srv = NULL;
    mutex = NULL;

done:
    if (handle) CloseHandle(handle);
    if (resource) resource->lpVtbl->Release(resource);
    if (adapter) adapter->lpVtbl->Release(adapter);
    if (dxgi_device) dxgi_device->lpVtbl->Release(dxgi_device);
    if (mutex) mutex->lpVtbl->Release(mutex);
    if (srv) srv->lpVtbl->Release(srv);
    if (texture) texture->lpVtbl->Release(texture);
    offer.status = (UINT32)hr;
    LeaveCriticalSection(&d->frame_cs);
    return idd_send_gpu(s, &offer);
}

static IDXGIAdapter *idd_gpu_adapter(const wchar_t *device_path)
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    PFND3DKMT_OPENADAPTERFROMDEVICENAME open_adapter = gdi
        ? (PFND3DKMT_OPENADAPTERFROMDEVICENAME)GetProcAddress(gdi, "D3DKMTOpenAdapterFromDeviceName") : NULL;
    PFND3DKMT_CLOSEADAPTER close_adapter = gdi
        ? (PFND3DKMT_CLOSEADAPTER)GetProcAddress(gdi, "D3DKMTCloseAdapter") : NULL;
    D3DKMT_OPENADAPTERFROMDEVICENAME opened = {0};
    D3DKMT_CLOSEADAPTER closed = {0};
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter *selected = NULL;
    NTSTATUS status;

    if (!device_path || !device_path[0] || !open_adapter || !close_adapter)
        return NULL;
    opened.pDeviceName = device_path;
    status = open_adapter(&opened);
    if (status < 0) {
        ui_log(L"IDD: selected GPU adapter lookup failed (0x%08X)", (UINT)status);
        return NULL;
    }
    closed.hAdapter = opened.hAdapter;
    close_adapter(&closed);

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory)))
        return NULL;
    factory->lpVtbl->EnumAdapterByLuid(factory, opened.AdapterLuid,
        &IID_IDXGIAdapter, (void **)&selected);
    factory->lpVtbl->Release(factory);
    return selected;
}

static HRESULT d3d_frame_texture(VmDisplayIdd *d)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    HRESULT hr;
    if (d->frame_tex) {
        d->frame_tex->lpVtbl->GetDesc(d->frame_tex, &desc);
        if (desc.Width == d->frame_width && desc.Height == d->frame_height) return S_OK;
    }
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = d->frame_width;
    desc.Height = d->frame_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = d->device->lpVtbl->CreateTexture2D(d->device, &desc, NULL, &texture);
    if (SUCCEEDED(hr))
        hr = d->device->lpVtbl->CreateShaderResourceView(d->device,
            (ID3D11Resource *)texture, NULL, &srv);
    if (FAILED(hr)) {
        if (texture) texture->lpVtbl->Release(texture);
        return hr;
    }
    if (d->frame_srv) d->frame_srv->lpVtbl->Release(d->frame_srv);
    if (d->frame_tex) d->frame_tex->lpVtbl->Release(d->frame_tex);
    d->frame_srv = srv;
    d->frame_tex = texture;
    return S_OK;
}

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    HRESULT hr;

    /* Create device and swap chain */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount                        = 2;
    scd.BufferDesc.Width                   = DEFAULT_WIDTH;
    scd.BufferDesc.Height                  = DEFAULT_HEIGHT;
    scd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = d->render_hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags                              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (!d->software_renderer) {
        IDXGIFactory5 *factory = NULL;
        BOOL supported = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory5, (void **)&factory))) {
            hr = factory->lpVtbl->CheckFeatureSupport(factory,
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported));
            if (SUCCEEDED(hr) && supported) scd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            factory->lpVtbl->Release(factory);
        }
    }

    {
        IDXGIAdapter *adapter = d->software_renderer ? NULL : idd_gpu_adapter(d->gpu_id);
        hr = E_FAIL;
        if (adapter) {
            hr = D3D11CreateDeviceAndSwapChain(
                adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                NULL, 0, D3D11_SDK_VERSION,
                &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);
            adapter->lpVtbl->Release(adapter);
            if (FAILED(hr)) {
                ui_log(L"IDD: selected GPU device creation failed (0x%08X); using raw display", hr);
                if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
                if (d->ctx) { d->ctx->lpVtbl->Release(d->ctx); d->ctx = NULL; }
                if (d->device) { d->device->lpVtbl->Release(d->device); d->device = NULL; }
            }
        }
        if (FAILED(hr)) {
            if (d->gpu_id[0]) d->shared_disabled = TRUE;
            hr = D3D11CreateDeviceAndSwapChain(
                NULL, d->software_renderer ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                NULL, 0, D3D11_SDK_VERSION,
                &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);
        }
        if (FAILED(hr) && !d->software_renderer) {
            if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
            if (d->ctx) { d->ctx->lpVtbl->Release(d->ctx); d->ctx = NULL; }
            if (d->device) { d->device->lpVtbl->Release(d->device); d->device = NULL; }
            d->software_renderer = TRUE;
            d->shared_disabled = TRUE;
            scd.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                NULL, 0, D3D11_SDK_VERSION, &scd, &d->swap_chain,
                &d->device, &feature_level, &d->ctx);
        }
    }

    if (FAILED(hr)) {
        ui_log(L"IDD: D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        return FALSE;
    }

    d->swap_flags = scd.Flags;

    {
        IDXGISwapChain2 *swap = NULL;
        hr = d->swap_chain->lpVtbl->QueryInterface(d->swap_chain, &IID_IDXGISwapChain2,
            (void **)&swap);
        if (SUCCEEDED(hr)) {
            hr = swap->lpVtbl->SetMaximumFrameLatency(swap, 1);
            if (SUCCEEDED(hr)) d->present_event = swap->lpVtbl->GetFrameLatencyWaitableObject(swap);
            swap->lpVtbl->Release(swap);
        }
        if (FAILED(hr) || !d->present_event) return FALSE;
    }

    /* Create render target view from back buffer */
    {
        ID3D11Texture2D *back_buf = NULL;
        hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                       &IID_ID3D11Texture2D, (void **)&back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: GetBuffer failed (0x%08X)", hr);
            return FALSE;
        }
        hr = d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: CreateRenderTargetView failed (0x%08X)", hr);
            return FALSE;
        }
    }

    hr = d3d_frame_texture(d);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateShaderResourceView failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create vertex shader */
    if (!d3d_compile_shader(g_vs_hlsl, "main", "vs_4_0", &vs_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreateVertexShader(d->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob),
            NULL, &d->vs);
    vs_blob->lpVtbl->Release(vs_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateVertexShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create pixel shader */
    if (!d3d_compile_shader(g_ps_hlsl, "main", "ps_4_0", &ps_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreatePixelShader(d->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob),
            NULL, &d->ps);
    ps_blob->lpVtbl->Release(ps_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreatePixelShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Sampler state (linear filtering) */
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = d->device->lpVtbl->CreateSamplerState(d->device, &sd, &d->sampler);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateSamplerState failed (0x%08X)", hr);
        return FALSE;
    }

    return TRUE;
}

static void d3d_resize_swap_chain(VmDisplayIdd *d)
{
    RECT rc;
    HRESULT hr;
    ID3D11Texture2D *back_buf = NULL;

    if (!d->swap_chain) return;

    /* Release old render target */
    if (d->rtv) {
        d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 0, NULL, NULL);
        d->rtv->lpVtbl->Release(d->rtv);
        d->rtv = NULL;
    }

    GetClientRect(d->render_hwnd, &rc);
    idd_log(d, L"Resize: render_hwnd client=%dx%d, frame=%ux%u",
            rc.right, rc.bottom, d->frame_width, d->frame_height);
    if (rc.right == 0 || rc.bottom == 0) {
        d->render_pending = FALSE;
        if (d->shared_front >= 0) {
            IddSharedSlot *front = &d->shared_slots[d->shared_front];
            if (d->shared_pending >= 0) {
                hr = idd_shared_unlock(d, front);
                if (hr != S_OK) idd_shared_fail(d);
            } else {
                d->shared_pending = d->shared_front;
                d->frame_dirty = TRUE;
            }
            d->shared_front = -1;
        }
        return;
    }

    hr = d->swap_chain->lpVtbl->ResizeBuffers(d->swap_chain, 0,
            (UINT)rc.right, (UINT)rc.bottom,
            DXGI_FORMAT_UNKNOWN, d->swap_flags);
    if (FAILED(hr)) {
        ui_log(L"IDD: ResizeBuffers failed (0x%08X)", hr);
        return;
    }

    hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                   &IID_ID3D11Texture2D, (void **)&back_buf);
    if (SUCCEEDED(hr)) {
        d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
    }
}

static void d3d_render_frame(VmDisplayIdd *d)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    D3D11_VIEWPORT vp;
    RECT rc;
    HRESULT hr;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    BOOL frame_uploaded = FALSE;
    BOOL shared = FALSE;
    ID3D11ShaderResourceView *frame_srv;
    ID3D11ShaderResourceView *null_srv = NULL;

    if (d->stop) return;
    if (!d->device || !d->ctx || !d->swap_chain) {
        if (GetTickCount64() >= d->render_retry_at) d3d_recover(d);
        return;
    }
    if (FAILED(d->device->lpVtbl->GetDeviceRemovedReason(d->device))) {
        d3d_recover(d);
        return;
    }
    if (!d->rtv) {
        GetClientRect(d->render_hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) d3d_recover(d);
        return;
    }

    if (d->present_event && !d->present_ready) {
        DWORD wait = WaitForSingleObject(d->present_event, 0);
        if (wait == WAIT_TIMEOUT) {
            d->render_pending = TRUE;
            return;
        }
        if (wait != WAIT_OBJECT_0) {
            d3d_recover(d);
            return;
        }
    }
    d->present_ready = FALSE;
    d->render_pending = FALSE;

    EnterCriticalSection(&d->frame_cs);
    shared = d->shared_ready;
    if (shared) {
        d->frame_dirty = FALSE;
        if (d->shared_pending >= 0 && !d->shared_slots[d->shared_pending].screenshot_yielded) {
            if (!d->shared_slots[d->shared_pending].acquired) goto shared_error;
            if (d->shared_front >= 0) {
                IddSharedSlot *old = &d->shared_slots[d->shared_front];
                hr = idd_shared_unlock(d, old);
                if (hr != S_OK) goto shared_error;
            }
            d->shared_front = d->shared_pending;
            d->shared_pending = -1;
            frame_uploaded = TRUE;
        }
        if (d->shared_front < 0 || !d->shared_slots[d->shared_front].acquired) {
            d->present_ready = d->present_event != NULL;
            LeaveCriticalSection(&d->frame_cs);
            return;
        }
        if (!d->frame_connected) {
            d->frame_connected = TRUE;
            PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
        }
    } else if (d->frame_dirty && d->frame_buf) {
        hr = d3d_frame_texture(d);
        if (SUCCEEDED(hr)) hr = d->ctx->lpVtbl->Map(d->ctx,
                (ID3D11Resource *)d->frame_tex, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            UINT row;
            UINT copy_stride = d->frame_width * 4;
            if (copy_stride > mapped.RowPitch)
                copy_stride = mapped.RowPitch;
            if (copy_stride > d->frame_stride)
                copy_stride = d->frame_stride;

            for (row = 0; row < d->frame_height; row++) {
                memcpy((BYTE *)mapped.pData + row * mapped.RowPitch,
                       d->frame_buf + row * d->frame_stride,
                       copy_stride);
            }
            d->ctx->lpVtbl->Unmap(d->ctx,
                    (ID3D11Resource *)d->frame_tex, 0);
        }
        if (FAILED(hr)) {
            LeaveCriticalSection(&d->frame_cs);
            d3d_recover(d);
            return;
        }
        d->frame_dirty = FALSE;
        frame_uploaded = TRUE;
    }

    /* Compute letterboxed viewport within client area */
    GetClientRect(d->render_hwnd, &rc);
    {
        float vp_x, vp_y, vp_w, vp_h;
        compute_letterbox((UINT)rc.right, (UINT)rc.bottom,
                          d->frame_width, d->frame_height,
                          &vp_x, &vp_y, &vp_w, &vp_h);
        ZeroMemory(&vp, sizeof(vp));
        vp.TopLeftX = vp_x;
        vp.TopLeftY = vp_y;
        vp.Width    = vp_w;
        vp.Height   = vp_h;
        vp.MaxDepth = 1.0f;
    }

    if (frame_uploaded && d->hwnd) {
        LONG size = MAKELONG(d->frame_width, d->frame_height);
        if (InterlockedExchange(&d->rendered_size, size) != size ||
            d->title_muted != d->audio_muted) {
            d->title_muted = d->audio_muted;
            PostMessageW(d->hwnd, WM_IDD_TITLE_CHANGED, 0, 0);
        }
    }
    d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
    d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &vp);
    d->ctx->lpVtbl->ClearRenderTargetView(d->ctx, d->rtv, clear_color);

    d->ctx->lpVtbl->IASetPrimitiveTopology(d->ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->lpVtbl->IASetInputLayout(d->ctx, NULL);

    d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
    d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps, NULL, 0);
    frame_srv = shared ? d->shared_slots[d->shared_front].srv : d->frame_srv;
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &frame_srv);
    d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);

    /* Draw fullscreen triangle (3 vertices, no vertex buffer) */
    d->ctx->lpVtbl->Draw(d->ctx, 3, 0);
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &null_srv);
    if (shared) {
        d->ctx->lpVtbl->Flush(d->ctx);
    }

    LeaveCriticalSection(&d->frame_cs);
    hr = d->swap_chain->lpVtbl->Present(d->swap_chain,
        (d->swap_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? 0 : 1,
        (d->swap_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) d3d_recover(d);
    return;

shared_error:
    d->present_ready = d->present_event != NULL;
    idd_shared_fail(d);
    LeaveCriticalSection(&d->frame_cs);
}

static void d3d_cleanup(VmDisplayIdd *d)
{
    if (d->present_event) CloseHandle(d->present_event);
    d->present_event = NULL;
    d->swap_flags = 0;
    d->present_ready = FALSE;
    d->render_pending = FALSE;
    idd_shared_release(d);
    if (d->ctx) d->ctx->lpVtbl->ClearState(d->ctx);
    if (d->sampler)    { d->sampler->lpVtbl->Release(d->sampler);       d->sampler = NULL; }
    if (d->ps)         { d->ps->lpVtbl->Release(d->ps);                 d->ps = NULL; }
    if (d->vs)         { d->vs->lpVtbl->Release(d->vs);                 d->vs = NULL; }
    if (d->frame_srv)  { d->frame_srv->lpVtbl->Release(d->frame_srv);   d->frame_srv = NULL; }
    if (d->frame_tex)  { d->frame_tex->lpVtbl->Release(d->frame_tex);   d->frame_tex = NULL; }
    if (d->rtv)        { d->rtv->lpVtbl->Release(d->rtv);               d->rtv = NULL; }
    if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
    if (d->ctx)        { d->ctx->lpVtbl->Release(d->ctx);               d->ctx = NULL; }
    if (d->device)     { d->device->lpVtbl->Release(d->device);         d->device = NULL; }
}

static void d3d_recover(VmDisplayIdd *d)
{
    EnterCriticalSection(&d->frame_cs);
    idd_shared_fail(d);
    d->software_renderer = TRUE;
    d3d_cleanup(d);
    if (!d3d_init(d)) d3d_cleanup(d);
    d->render_retry_at = GetTickCount64() + 1000;
    d->frame_dirty = TRUE;
    LeaveCriticalSection(&d->frame_cs);
}

static void d3d_refresh_gpu(VmDisplayIdd *d)
{
    EnterCriticalSection(&d->frame_cs);
    d3d_cleanup(d);
    d->software_renderer = FALSE;
    d->shared_disabled = FALSE;
    if (!d3d_init(d)) {
        d3d_cleanup(d);
        d->software_renderer = TRUE;
        d->shared_disabled = TRUE;
        d->render_retry_at = GetTickCount64() + 1000;
    } else {
        d3d_resize_swap_chain(d);
    }
    d->frame_dirty = TRUE;
    InterlockedExchange(&d->gpu_refresh_requested, 0);
    WakeAllConditionVariable(&d->gpu_refresh_done);
    LeaveCriticalSection(&d->frame_cs);
}

/* ==================================================================
 * Guest cursor — create HCURSOR from received bitmap
 * ================================================================== */

/* Cursor types from IddCx IDDCX_CURSOR_SHAPE_TYPE */
#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

static HCURSOR create_cursor_from_bitmap(UINT width, UINT height,
                                          UINT xhot, UINT yhot,
                                          UINT cursor_type, UINT pitch,
                                          UINT shape_data_size,
                                          const BYTE *shape_data)
{
    HCURSOR result = NULL;
    BITMAPINFO bmi;
    HBITMAP hColor = NULL, hMask = NULL;
    ICONINFO ii;
    HDC hdc;

    if (width == 0 || height == 0 || width > 256 || height > 256)
        return NULL;

    /* shape_data holds exactly shape_data_size bytes; both copy paths read up
       to (height-1)*pitch + width*4 bytes. Require the stride to cover a row
       and the total to cover all rows, else a tiny buffer with large
       width/height/pitch would over-read. */
    if (pitch < width * 4 || (UINT64)pitch * height > (UINT64)shape_data_size)
        return NULL;

    hdc = GetDC(NULL);

    if (cursor_type == CURSOR_TYPE_MASKED_COLOR) {
        /* MASKED_COLOR (IddCx 1.10 / QueryHardwareCursor3): single 32bpp BGRA
           image where the alpha channel encodes the AND mask.
           Height is the ACTUAL cursor height (not doubled).
           Per pixel: A = AND mask (0xFF = transparent/XOR, 0x00 = opaque),
                      B,G,R = XOR color values.
           We extract A into a 1bpp monochrome hbmMask and BGR into hbmColor. */
        UINT mask_row_bytes = (width + 7) / 8;
        /* CreateBitmap consumes WORD-aligned rows, unlike a DWORD-aligned
           DIB. */
        UINT mask_pitch = (mask_row_bytes + 1) & ~1u;
        void *color_bits = NULL;
        BYTE *mask_buf;
        UINT row, col;

        /* Build 1bpp AND mask from alpha channel */
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      mask_pitch * height);
        if (!mask_buf) { ReleaseDC(NULL, hdc); return NULL; }

        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                BYTE alpha = src_row[col * 4 + 3];  /* A channel */
                if (alpha != 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        /* XOR color bitmap (32bpp, top-down) — copy full BGRA data */
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)width;
        bmi.bmiHeader.biHeight      = -(LONG)height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (hColor && color_bits) {
            UINT dst_pitch = width * 4;
            for (row = 0; row < height; row++) {
                const BYTE *src = shape_data + row * pitch;
                BYTE *dst = (BYTE *)color_bits + row * dst_pitch;
                for (col = 0; col < width; col++) {
                    dst[col * 4 + 0] = src[col * 4 + 0];  /* B */
                    dst[col * 4 + 1] = src[col * 4 + 1];  /* G */
                    dst[col * 4 + 2] = src[col * 4 + 2];  /* R */
                    dst[col * 4 + 3] = 0;                  /* A = 0, AND mask handles transparency */
                }
            }
        }

        ReleaseDC(NULL, hdc);

        if (!hColor || !hMask) {
            if (hColor) DeleteObject(hColor);
            if (hMask)  DeleteObject(hMask);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;

    } else {
        /* ALPHA (type 2): 32bpp BGRA with premultiplied alpha.
           Height is the real cursor height.

           The 32-bpp DIB section with BI_RGB doesn't tell Windows that
           the top byte is alpha — Windows treats it as 24-bit RGB +
           padding and, with an all-zero AND mask, renders EVERY pixel
           opaque. For cursors whose transparent regions are stored as
           (0,0,0,0) (e.g. ours from Linux), that paints a black square
           around the cursor. Windows-VDD cursors happen not to trigger
           this because their transparent regions have non-zero RGB.

           Fix: build the AND mask from the alpha channel — bit set
           (1) = transparent, bit cleared (0) = opaque. Threshold low
           (any alpha > 0 = opaque) preserves anti-aliased edges, the
           alpha channel then handles smooth blending of those edges.

           Use BITMAPV4HEADER with explicit alpha mask instead of
           BITMAPINFOHEADER+BI_RGB. The latter is ambiguous at 32-bpp:
           some Windows paths treat it as XRGB (alpha ignored, RGB
           rendered at full opacity → cursor body looks too bright).
           V4 with bV4AlphaMask = 0xFF000000 settles it. */
        void *color_bits = NULL;
        UINT row, col, dst_pitch;
        UINT mask_row_bytes, mask_pitch;
        BYTE *mask_buf;
        BITMAPV4HEADER bv4;

        ZeroMemory(&bv4, sizeof(bv4));
        bv4.bV4Size          = sizeof(BITMAPV4HEADER);
        bv4.bV4Width         = (LONG)width;
        bv4.bV4Height        = -(LONG)height;
        bv4.bV4Planes        = 1;
        bv4.bV4BitCount      = 32;
        bv4.bV4V4Compression = BI_BITFIELDS;
        bv4.bV4RedMask       = 0x00FF0000;
        bv4.bV4GreenMask     = 0x0000FF00;
        bv4.bV4BlueMask      = 0x000000FF;
        bv4.bV4AlphaMask     = 0xFF000000;

        hColor = CreateDIBSection(hdc, (BITMAPINFO *)&bv4,
                                  DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (!hColor || !color_bits) {
            ReleaseDC(NULL, hdc);
            return NULL;
        }

        dst_pitch = width * 4;
        for (row = 0; row < height; row++) {
            memcpy((BYTE *)color_bits + row * dst_pitch,
                   shape_data + row * pitch,
                   dst_pitch);
        }

        /* AND mask, 1bpp, WORD-aligned for CreateBitmap. Bit set = transparent. */
        mask_row_bytes = (width + 7) / 8;
        mask_pitch     = (mask_row_bytes + 1) & ~1u;
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (size_t)mask_pitch * height);
        if (!mask_buf) {
            DeleteObject(hColor);
            ReleaseDC(NULL, hdc);
            return NULL;
        }
        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                if (src_row[col * 4 + 3] == 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        ReleaseDC(NULL, hdc);

        if (!hMask) {
            DeleteObject(hColor);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
    }

    result = (HCURSOR)CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);

    return result;
}

/* ==================================================================
 * Recv thread - connects to VM, receives frames, updates frame_buf
 * ================================================================== */

static void clip_log_callback(const wchar_t *msg, void *user_data)
{
    VmDisplayIdd *d = (VmDisplayIdd *)user_data;
    idd_log(d, L"%s", msg);
}

static DWORD WINAPI idd_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;
    AsbGpuBroker *broker = NULL;
    HANDLE input_thread;
    UINT recv_capacity = DEFAULT_WIDTH * DEFAULT_HEIGHT * 4;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Allocate receive buffer for frame pixel data */
    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, recv_capacity);
    if (!recv_buf) {
        ui_log(L"IDD recv: failed to allocate receive buffer");
        return 1;
    }

    /* Tell the agent to respawn input helper in console session */
    VmInstance *vm = asb_find_vm_by_id(d->vm_id);
    if (!d->stop && vm && vm->agent_online) {
        idd_log(d, L"Sending idd_connect to agent...");
        vm_agent_send(vm, "idd_connect", NULL, 0, 5000);
    }

    input_thread = CreateThread(NULL, 0, idd_input_thread_proc, d, 0, NULL);
    if (!input_thread) {
        ui_log(L"IDD: Failed to create input thread.");
        HeapFree(GetProcessHeap(), 0, recv_buf);
        WSACleanup();
        PostMessageW(d->hwnd, WM_CLOSE, 0, 0);
        return 1;
    }

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;

        /* Clipboard module (handles :0005 + :0006 internally) */
        if (!d->clipboard) {
            d->clipboard = vm_clipboard_create(&d->runtime_id, d->os_type,
                                               d->hwnd, clip_log_callback, d);
            if (d->clipboard)
                idd_log(d, L"Clipboard module created.");
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        {
            GUID svc; hcs_service_guid(d->os_type, 2, &svc);
            s = connect_to_hv_service(&d->runtime_id, &svc, 3000);
        }
        if (s == INVALID_SOCKET) {
            int wait;
            idd_log(d, L"Connection failed, retrying in 3s.");
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        vm = asb_find_vm_by_id(d->vm_id);
        EnterCriticalSection(&d->frame_cs);
        if (!d->stop && d->shared_thread && vm && vm->running &&
            wcscmp(d->gpu_id, vm->gpu_id) != 0) {
            wcscpy_s(d->gpu_id, ARRAYSIZE(d->gpu_id), vm->gpu_id);
            InterlockedExchange(&d->gpu_refresh_requested, 1);
            SetEvent(d->shared_wake);
            while (d->gpu_refresh_requested && !d->stop)
                SleepConditionVariableCS(&d->gpu_refresh_done, &d->frame_cs, 100);
        }
        if (d->stop) {
            LeaveCriticalSection(&d->frame_cs);
            closesocket(s);
            break;
        }
        d->frame_socket = s;
        d->shared_offered = FALSE;
        d->shared_rejected = FALSE;
        d->shared_session = 0;
        LeaveCriticalSection(&d->frame_cs);
        if (vm && vm->gpu_mode == GPU_DEFAULT && !d->shared_disabled) {
            static volatile LONG gpu_session_id;
            DisplayGpuPacket query = {0};
            DWORD send_timeout = 1000;
            query.magic = DISPLAY_GPU_MAGIC;
            query.type = DISPLAY_GPU_QUERY;
            query.version = DISPLAY_GPU_VERSION;
            query.reserved = _wcsicmp(d->os_type, L"windows") == 0
                ? DISPLAY_GPU_CAP_RESIZE : DISPLAY_GPU_CAP_FENCE | DISPLAY_GPU_CAP_RESIZE;
            query.reserved |= DISPLAY_GPU_CAP_REFRESH;
            query.reserved |= DISPLAY_GPU_CAP_SNAPSHOT;
            query.session = ((UINT64)GetCurrentProcessId() << 32) |
                            (UINT32)InterlockedIncrement(&gpu_session_id);
            d->shared_session = query.session;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));
            if (!idd_send_gpu(s, &query)) shutdown(s, SD_BOTH);
        }

        idd_log(d, L"Frame channel connected.");
        d->cursor_visible = TRUE;
        PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;
            BOOL resolution_changed = FALSE;

            /* Peek at magic to determine message type */
            if (!recv_exact(d, s, &magic, sizeof(magic), d->shared_ready))
                break;

            if (magic == DISPLAY_GPU_MAGIC) {
                DisplayGpuPacket packet;
                packet.magic = magic;
                if (!recv_exact(d, s, (BYTE *)&packet + sizeof(magic), sizeof(packet) - sizeof(magic), FALSE))
                    break;
                if (packet.version != DISPLAY_GPU_VERSION) {
                    d->shared_disabled = TRUE;
                    break;
                }
                if (!d->shared_session || packet.session != d->shared_session) break;
                if (packet.type == DISPLAY_GPU_CAPS) {
                    UINT slot;
                    if (d->shared_offered) break;
                    d->shared_offered = TRUE;
                    if (_wcsicmp(d->os_type, L"windows") != 0 &&
                        (packet.reserved & DISPLAY_GPU_CAP_FENCE)) {
                        if (!idd_shared_start_linux(d, s, &packet, &broker)) break;
                        continue;
                    }
                    EnterCriticalSection(&d->frame_cs);
                    d->shared_resize = (packet.reserved & DISPLAY_GPU_CAP_RESIZE) != 0;
                    d->shared_refresh = (packet.reserved & DISPLAY_GPU_CAP_REFRESH) != 0;
                    d->shared_screenshot = (packet.reserved & DISPLAY_GPU_CAP_SNAPSHOT) != 0;
                    d->shared_refresh_rate = d->shared_refresh ? (UINT)packet.handle : 60;
                    d->shared_width = packet.width;
                    d->shared_height = packet.height;
                    d->guest_size_mode = packet.slot;
                    LeaveCriticalSection(&d->frame_cs);
                    for (slot = 0; slot < DISPLAY_GPU_SLOTS; ++slot) {
                        if (!idd_shared_offer(d, s, &packet, slot)) break;
                    }
                    if (slot != DISPLAY_GPU_SLOTS) break;
                } else if (packet.type == DISPLAY_GPU_OFFER && broker) {
                    if (!idd_shared_import_linux(d, s, &packet, broker)) break;
                } else if (packet.type == DISPLAY_GPU_READY && !broker) {
                    BOOL ready = TRUE;
                    UINT slot;
                    if (packet.slot >= DISPLAY_GPU_SLOTS) break;
                    EnterCriticalSection(&d->frame_cs);
                    if (packet.status != S_OK) {
                        d->shared_rejected = TRUE;
                        idd_shared_release(d);
                        LeaveCriticalSection(&d->frame_cs);
                        continue;
                    }
                    if (d->shared_disabled || d->shared_rejected || d->shared_ready || !d->shared_offered) {
                        LeaveCriticalSection(&d->frame_cs);
                        break;
                    }
                    d->shared_slots[packet.slot].ready = d->shared_slots[packet.slot].texture != NULL;
                    for (slot = 0; slot < DISPLAY_GPU_SLOTS; ++slot)
                        ready = ready && d->shared_slots[slot].ready;
                    d->shared_ready = ready;
                    if (ready) SetEvent(d->shared_wake);
                    LeaveCriticalSection(&d->frame_cs);
                } else if (packet.type == DISPLAY_GPU_SNAPSHOT && !broker) {
                    HRESULT status = S_FALSE;
                    EnterCriticalSection(&d->frame_cs);
                    if (d->shared_screenshot && d->shared_ready && packet.slot < DISPLAY_GPU_SLOTS) {
                        IddSharedSlot *slot = &d->shared_slots[packet.slot];
                        if (slot->acquired && slot->mutex && !slot->screenshot_yielded) {
                            status = slot->mutex->lpVtbl->ReleaseSync(slot->mutex, 2);
                            if (status == S_OK) {
                                slot->acquired = FALSE;
                                slot->screenshot_yielded = GetTickCount64();
                                SetEvent(d->shared_wake);
                            }
                        }
                    }
                    LeaveCriticalSection(&d->frame_cs);
                    packet.status = status;
                    if (!idd_send_gpu(s, &packet)) break;
                } else if (packet.type == DISPLAY_GPU_RESIZED) {
                    if (packet.status != S_OK) {
                        d->size_reconnect_until = 0;
                        idd_log(d, L"Display resize rejected (0x%08X).", packet.status);
                        if (broker) PostMessageW(d->hwnd, WM_IDD_DISPLAY_SIZE, TRUE, packet.status);
                    } else if (broker && packet.slot <= DISPLAY_SIZE_WINDOW) {
                        EnterCriticalSection(&d->frame_cs);
                        d->guest_size_mode = packet.slot;
                        if (d->shared_refresh && packet.handle) d->shared_refresh_rate = (UINT)packet.handle;
                        LeaveCriticalSection(&d->frame_cs);
                    }
                } else {
                    break;
                }
                continue;
            }

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(d, s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32), FALSE))
                    break;

                BOOL cursor_changed = d->cursor_visible != (chdr.visible != 0);
                d->cursor_visible = chdr.visible != 0;

                if (chdr.shape_updated && chdr.shape_data_size > 0) {
                    BYTE *cursor_buf;
                    if (chdr.shape_data_size > MAX_CURSOR_SIZE) {
                        idd_log(d, L"Cursor data too large (%u), reconnecting.",
                               chdr.shape_data_size);
                        break;
                    }
                    cursor_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                                    chdr.shape_data_size);
                    if (!cursor_buf) break;

                    if (!recv_exact(d, s, cursor_buf, (int)chdr.shape_data_size, FALSE)) {
                        HeapFree(GetProcessHeap(), 0, cursor_buf);
                        break;
                    }

                    /* Create new cursor from bitmap */
                    {
                        HCURSOR new_cursor = create_cursor_from_bitmap(
                            chdr.width, chdr.height, chdr.xhot, chdr.yhot,
                            chdr.cursor_type, chdr.pitch,
                            chdr.shape_data_size, cursor_buf);
                        if (new_cursor) {
                            HCURSOR old = d->guest_cursor;
                            d->guest_cursor = new_cursor;
                            d->cursor_shape_id = chdr.shape_id;
                            if (old) DestroyCursor(old);
                            cursor_changed = TRUE;
                        }
                    }

                    HeapFree(GetProcessHeap(), 0, cursor_buf);
                }
                if (cursor_changed)
                    PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
                continue;  /* back to message loop */
            }

            if (magic != FRAME_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }

            /* Read rest of frame header (already read magic) */
            hdr.magic = magic;
            if (!recv_exact(d, s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32), FALSE))
                break;

            /* Sanity checks */
            if (hdr.width == 0 || hdr.height == 0 ||
                hdr.width > 7680 || hdr.height > 4320 ||
                hdr.stride < hdr.width * 4) {
                idd_log(d, L"Invalid frame dimensions %ux%u stride %u.",
                       hdr.width, hdr.height, hdr.stride);
                break;
            }

            rect_count = hdr.dirty_rect_count;
            if (rect_count > MAX_DIRTY_RECTS) {
                idd_log(d, L"Too many dirty rects (%u), reconnecting.", rect_count);
                break;
            }

            /* Read dirty rects */
            if (rect_count > 0) {
                if (!recv_exact(d, s, dirty_rects, (int)(rect_count * sizeof(RECT)), FALSE))
                    break;
            }

            /* Read data_size */
            if (!recv_exact(d, s, &data_size, 4, FALSE))
                break;

            if (data_size > MAX_FRAME_DATA_SIZE) {
                idd_log(d, L"Frame data too large (%u bytes), reconnecting.", data_size);
                break;
            }
            if (data_size > recv_capacity) {
                BYTE *new_buf = (BYTE *)HeapReAlloc(GetProcessHeap(), 0, recv_buf, data_size);
                if (!new_buf) break;
                recv_buf = new_buf;
                recv_capacity = data_size;
            }

            /* Read pixel data */
            if (data_size > 0) {
                if (!recv_exact(d, s, recv_buf, (int)data_size, FALSE))
                    break;
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            /* Reallocate frame_buf if resolution changed */
            if (!d->frame_buf || hdr.width != d->frame_width || hdr.height != d->frame_height) {
                UINT new_stride = hdr.width * 4;
                UINT new_size   = new_stride * hdr.height;
                BYTE *new_buf   = (BYTE *)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY, new_size);
                if (new_buf) {
                    if (d->frame_buf)
                        HeapFree(GetProcessHeap(), 0, d->frame_buf);
                    d->frame_buf    = new_buf;
                    d->frame_width  = hdr.width;
                    d->frame_height = hdr.height;
                    d->frame_stride = new_stride;
                    resolution_changed = TRUE;
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
                /* Bound source rows by the bytes actually received: width/height/
                   stride and data_size are independent guest-controlled fields, so
                   only data_size/stride rows of recv_buf hold valid pixels. */
                UINT src_rows = hdr.stride ? (UINT)(data_size / hdr.stride) : 0;
                if (copy_w > d->frame_stride) copy_w = d->frame_stride;
                if (copy_w > hdr.stride)      copy_w = hdr.stride;

                for (row = 0; row < hdr.height && row < d->frame_height &&
                              row < src_rows; row++) {
                    memcpy(d->frame_buf + row * d->frame_stride,
                           src + row * hdr.stride,
                           copy_w);
                }
            } else {
                /* Dirty rect updates — pixel data is per-rect rows concatenated */
                BYTE *src = recv_buf;
                for (i = 0; i < rect_count; i++) {
                    LONG left   = dirty_rects[i].left;
                    LONG top    = dirty_rects[i].top;
                    LONG right  = dirty_rects[i].right;
                    LONG bottom = dirty_rects[i].bottom;
                    UINT rect_w, rect_h, row;
                    UINT rect_row_bytes;

                    /* Clamp to frame bounds */
                    if (left < 0) left = 0;
                    if (top  < 0) top  = 0;
                    if (right  > (LONG)d->frame_width)  right  = (LONG)d->frame_width;
                    if (bottom > (LONG)d->frame_height) bottom = (LONG)d->frame_height;
                    if (left >= right || top >= bottom) continue;

                    rect_w = (UINT)(right - left);
                    rect_h = (UINT)(bottom - top);
                    rect_row_bytes = rect_w * 4;

                    /* Refuse to read past the bytes actually received into recv_buf. */
                    if ((size_t)(src - recv_buf) + (size_t)rect_row_bytes * rect_h >
                        (size_t)data_size)
                        break;

                    for (row = 0; row < rect_h; row++) {
                        UINT dst_y = (UINT)top + row;
                        memcpy(d->frame_buf + dst_y * d->frame_stride + (UINT)left * 4,
                               src + row * rect_row_bytes,
                               rect_row_bytes);
                    }
                    src += rect_row_bytes * rect_h;
                }
            }

            d->frame_dirty = TRUE;
            if (d->display_width != d->frame_width || d->display_height != d->frame_height) {
                d->display_width = d->frame_width;
                d->display_height = d->frame_height;
                PostMessageW(d->hwnd, WM_IDD_DISPLAY_SIZE, TRUE, 0);
            }
            LeaveCriticalSection(&d->frame_cs);
            if (resolution_changed) {
                idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                        hdr.width, hdr.height, hdr.stride);
                idd_log(d, L"Resolution changed to %ux%u.", hdr.width, hdr.height);
            }
            if (!d->frame_connected) {
                d->frame_connected = TRUE;
                PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
            }

            /* Signal the window thread to repaint */
            if (d->hwnd && IsWindow(d->hwnd))
                PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);
        }

        /* Frame channel lost — close it but keep input alive */
        EnterCriticalSection(&d->frame_cs);
        if (d->shared_ready && !d->shared_disabled)
            d->size_reconnect_until = GetTickCount64() + 10000;
        d->frame_socket = INVALID_SOCKET;
        idd_shared_release(d);
        LeaveCriticalSection(&d->frame_cs);
        asb_gpu_broker_close(broker);
        broker = NULL;
        d->frame_connected = FALSE;
        d->cursor_visible = TRUE;
        PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
        closesocket(s);
        idd_log(d, L"Frame channel disconnected, reconnecting...");

        /* Wait before reconnecting frame channel */
        {
            int wait;
            int delay = GetTickCount64() < d->size_reconnect_until ? 100 : 3000;
            for (wait = 0; wait < delay && !d->stop; wait += 100)
                Sleep(100);
        }
    }

    WaitForSingleObject(input_thread, INFINITE);
    CloseHandle(input_thread);

    if (recv_buf)
        HeapFree(GetProcessHeap(), 0, recv_buf);

    WSACleanup();
    return 0;
}

/* ==================================================================
 * Window thread — creates window, initializes D3D11, runs message pump
 * ================================================================== */

static DWORD WINAPI idd_window_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    wchar_t title[300];
    MSG msg;
    HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    d->window_thread_id = GetCurrentThreadId();

    ensure_idd_class(d->hInstance);

    swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);

    /* Compute outer window size so the client area is exactly 1920x1080 */
    {
        DWORD style   = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
        DWORD exstyle = 0;
        RECT wr = { 0, 0, 1920, 1080 };
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);

        d->hwnd = CreateWindowExW(
            exstyle, IDD_DISPLAY_CLASS, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, d->hInstance, d);
    }

    if (!d->hwnd) {
        ui_log(L"IDD: CreateWindowEx failed (0x%08X)", GetLastError());
        d->open = FALSE;
        if (SUCCEEDED(apartment)) CoUninitialize();
        return 1;
    }

    /* Dark mode title bar to match AppSandbox main window */
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* Add options to the system menu (right-click title bar) */
    {
        HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
        if (sysmenu) {
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_AUDIO_MUTE, L"Mute audio");
            AppendMenuW(sysmenu, MF_STRING, IDM_XMIT_HOTKEYS, L"Transmit Keyboard Hotkeys");
            AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_LOG, L"Show Log");
            AppendMenuW(sysmenu, MF_STRING | MF_GRAYED, IDM_MATCH_WINDOW, L"Match window size");
            CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                          MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    /* Bring the display window to the foreground on open */
    ShowWindow(d->hwnd, SW_SHOW);
    BringWindowToTop(d->hwnd);
    SetForegroundWindow(d->hwnd);

    /* Render child fills entire client area */
    {
        RECT rc;
        GetClientRect(d->hwnd, &rc);

        d->render_hwnd = CreateWindowExW(
            0, IDD_RENDER_CLASS, NULL,
            WS_CHILD | WS_VISIBLE,
            0, 0, rc.right, rc.bottom,
            d->hwnd, NULL, d->hInstance, NULL);
    }

    /* Separate top-level log window */
    {
        wchar_t log_title[300];
        HFONT font;
        swprintf_s(log_title, 300, L"%s - IDD Log", d->vm_name);

        /* Created hidden — shown on demand via the "Show Log" system-menu item. */
        d->log_hwnd = CreateWindowExW(
            0, L"AppSandboxIddLog", log_title,
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT, CW_USEDEFAULT, LOG_WINDOW_W, LOG_WINDOW_H,
            NULL, NULL, d->hInstance, NULL);

        if (d->log_hwnd) {
            /* Dark mode title bar to match AppSandbox main window */
            BOOL dark = TRUE;
            DwmSetWindowAttribute(d->log_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            /* Fill the log window with a listbox */
            RECT lrc;
            GetClientRect(d->log_hwnd, &lrc);
            d->log_list_hwnd = CreateWindowExW(
                0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_HASSTRINGS,
                0, 0, lrc.right, lrc.bottom,
                d->log_hwnd, (HMENU)(INT_PTR)IDC_LOG_LIST, d->hInstance, NULL);

            /* Subclass the listbox so Ctrl+A / Ctrl+C work when it has focus */
            if (d->log_list_hwnd)
                g_orig_listbox_proc = (WNDPROC)SetWindowLongPtrW(
                    d->log_list_hwnd, GWLP_WNDPROC, (LONG_PTR)idd_log_listbox_proc);

            font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
            if (font && d->log_list_hwnd)
                SendMessageW(d->log_list_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
        }
        idd_log(d, L"IDD display started.");
    }

    /* Initialize D3D11 */
    if (!d3d_init(d)) {
        ui_log(L"IDD: D3D11 initialization failed.");
        d3d_cleanup(d);
        d->software_renderer = TRUE;
        d->shared_disabled = TRUE;
        d->render_retry_at = GetTickCount64() + 1000;
    }
    if (SUCCEEDED(apartment))
        d->agent_overlay = agent_overlay_create(d->render_hwnd, d->hwnd, WM_IDD_TAKE_CONTROL,d->vm_id);

    /* Start the recv thread now that the window and D3D11 are ready */
    d->shared_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->shared_fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (d->shared_wake)
        d->shared_thread = CreateThread(NULL, 0, idd_shared_thread_proc, d, 0, NULL);
    if (!d->shared_thread) d->shared_disabled = TRUE;
    d->recv_thread = CreateThread(NULL, 0, idd_recv_thread_proc, d, 0, NULL);
    if (!d->recv_thread) {
        ui_log(L"IDD: Failed to create recv thread.");
        d->stop = TRUE;
        if (d->shared_wake) SetEvent(d->shared_wake);
        if (d->shared_thread) {
            WaitForSingleObject(d->shared_thread, INFINITE);
            CloseHandle(d->shared_thread);
            d->shared_thread = NULL;
        }
        if (d->shared_fence_event) CloseHandle(d->shared_fence_event);
        d->shared_fence_event = NULL;
        if (d->shared_wake) CloseHandle(d->shared_wake);
        d->shared_wake = NULL;
        agent_overlay_destroy(d->agent_overlay);
        d->agent_overlay = NULL;
        d3d_cleanup(d);
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        if (SUCCEEDED(apartment)) CoUninitialize();
        return 1;
    }

    /* Start a present timer for steady rendering */
    SetTimer(d->hwnd, IDT_PRESENT, PRESENT_MS, NULL);

    idd_update_kbd_hook(d);

    if (d->stop)
        PostMessageW(d->hwnd, WM_CLOSE, 0, 0);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return 0;
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

static void idd_join_thread(HANDLE thread)
{
    while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    }
}

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        AddClipboardFormatListener(hwnd);
        return 0;
    }

    d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (d && (msg==WM_MOUSEMOVE || msg==WM_NCMOUSEMOVE)) agent_overlay_pointer_activity(d->agent_overlay);
    if (d && !d->mouse_buttons && agent_overlay_mouse(d->agent_overlay, msg, wp, lp)) return 0;

    if (d && d->mouse_sync_pending &&
        (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
         msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL))
        idd_resume_absolute_mouse(d, NULL);

    switch (msg) {
    case WM_IDD_TAKE_CONTROL:
        if (d) {
            if (wp == ASB_INPUT_USER_GRANT || wp == ASB_INPUT_USER_REVOKE || wp == ASB_INPUT_USER_CANCEL) {
                d->pending_agent_command=(UINT)wp;
                d->pending_agent_work=(UINT32)lp;
            }
        }
        return 0;
    case WM_INITMENU:
        if (d) {
            BOOL enabled;
            HMENU menu = GetSystemMenu(hwnd, FALSE);
            enabled = d->shared_ready && d->shared_resize && !d->shared_disabled &&
                (d->shared_front >= 0 || d->shared_pending >= 0);
            EnableMenuItem(menu, IDM_MATCH_WINDOW, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
            CheckMenuItem(menu, IDM_MATCH_WINDOW, MF_BYCOMMAND |
                (d->size_mode == DISPLAY_SIZE_WINDOW ? MF_CHECKED : MF_UNCHECKED));
        }
        break;
    case WM_SYSCOMMAND:
        if (d && (wp & 0xFFF0) == IDM_MATCH_WINDOW) {
            BOOL enabled;
            enabled = d->shared_ready && d->shared_resize && !d->shared_disabled &&
                (d->shared_front >= 0 || d->shared_pending >= 0);
            if (enabled) {
                d->size_mode = d->size_mode == DISPLAY_SIZE_WINDOW ? DISPLAY_SIZE_GUEST : DISPLAY_SIZE_WINDOW;
            }
            if (enabled) idd_update_display_size(d, TRUE);
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_AUDIO_MUTE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            wchar_t title[300];
            d->audio_muted = !d->audio_muted;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_AUDIO_MUTE,
                              MF_BYCOMMAND | (d->audio_muted ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->audio_muted)
                swprintf_s(title, 300, L"\U0001F507 %s - IDD Display", d->vm_name);
            else
                swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);
            SetWindowTextW(hwnd, title);
            idd_log(d, d->audio_muted ? L"Audio muted." : L"Audio unmuted.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_XMIT_HOTKEYS) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->transmit_hotkeys = !d->transmit_hotkeys;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                              MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            }
            if (!d->transmit_hotkeys) {
                /* Release anything the guest may be holding from this mode. */
                idd_flush_held_keys(d);
            }
            idd_update_kbd_hook(d);
            idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys);
            idd_log(d, d->transmit_hotkeys
                        ? L"Transmit Keyboard Hotkeys: ON."
                        : L"Transmit Keyboard Hotkeys: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_SHOW_LOG) {
            /* Reveal the log window (hidden by default). Closing it via its
               own [X] just hides it again (see idd_log_proc WM_CLOSE), so
               this item can re-open it any number of times. */
            if (d->log_hwnd && IsWindow(d->log_hwnd)) {
                if (IsIconic(d->log_hwnd))
                    ShowWindow(d->log_hwnd, SW_RESTORE);
                else
                    ShowWindow(d->log_hwnd, SW_SHOW);
                BringWindowToTop(d->log_hwnd);
                SetForegroundWindow(d->log_hwnd);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            RemoveClipboardFormatListener(hwnd);

            /* Remove the hotkey hook and release any keys still held in the
               guest before tearing the window down. */
            idd_remove_kbd_hook(d);
            idd_flush_held_keys(d);
            idd_flush_mouse_buttons(d);

            /* Stop recv threads */
            d->stop = TRUE;
            idd_update_relative_mouse(d);
            if (d->shared_wake) SetEvent(d->shared_wake);
            if (d->shared_thread) {
                idd_join_thread(d->shared_thread);
                CloseHandle(d->shared_thread);
                d->shared_thread = NULL;
            }

            EnterCriticalSection(&d->frame_cs);
            if (d->frame_socket != INVALID_SOCKET)
                shutdown(d->frame_socket, SD_BOTH);
            if (d->audio_socket != INVALID_SOCKET)
                shutdown(d->audio_socket, SD_BOTH);
            LeaveCriticalSection(&d->frame_cs);
            if (d->recv_thread) {
                idd_join_thread(d->recv_thread);
                CloseHandle(d->recv_thread);
                d->recv_thread = NULL;
            }

            /* Destroy clipboard module */
            if (d->clipboard) {
                vm_clipboard_destroy(d->clipboard);
                d->clipboard = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                idd_join_thread(d->audio_recv_thread);
                CloseHandle(d->audio_recv_thread);
                d->audio_recv_thread = NULL;
            }

            d->open = FALSE;

            /* Close the separate log window */
            if (d->log_hwnd && IsWindow(d->log_hwnd))
                DestroyWindow(d->log_hwnd);
            d->log_hwnd = NULL;
            d->log_list_hwnd = NULL;

            /* Clean up D3D11 */
            agent_overlay_destroy(d->agent_overlay);
            d->agent_overlay = NULL;
            d3d_cleanup(d);
            if (d->shared_fence_event) CloseHandle(d->shared_fence_event);
        d->shared_fence_event = NULL;
        if (d->shared_wake) CloseHandle(d->shared_wake);
            d->shared_wake = NULL;

            /* Clean up guest cursor */
            if (d->guest_cursor) {
                DestroyCursor(d->guest_cursor);
                d->guest_cursor = NULL;
            }

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm_id)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             (WPARAM)d->token, (LPARAM)d->vm_id);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PRESENT);
        if (d) idd_remove_kbd_hook(d);  /* safety net if WM_CLOSE was bypassed */
        if (d) {
            agent_overlay_destroy(d->agent_overlay);
            d->agent_overlay = NULL;
            d->stop = TRUE;
            idd_update_relative_mouse(d);
        }
        if (d) d->hwnd = NULL;
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        RECT wr;
        BOOL match_window = FALSE;
        if (d) {
            match_window = d->size_mode == DISPLAY_SIZE_WINDOW &&
                d->shared_ready && d->shared_resize && !d->shared_disabled;
        }
        wr.left = 0; wr.top = 0;
        wr.right = match_window ? DISPLAY_MIN_WIDTH : 320;
        wr.bottom = match_window ? DISPLAY_MIN_HEIGHT : 180;
        AdjustWindowRectExForDpi(&wr, style, FALSE, exstyle, GetDpiForWindow(hwnd));
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;
        if (d && d->frame_width > 0 && d->frame_height > 0) {
            wr.left = 0; wr.top = 0;
            wr.right = (LONG)(match_window ? DISPLAY_MAX_WIDTH : d->frame_width);
            wr.bottom = (LONG)(match_window ? DISPLAY_MAX_HEIGHT : d->frame_height);
            AdjustWindowRectExForDpi(&wr, style, FALSE, exstyle, GetDpiForWindow(hwnd));
            mmi->ptMaxTrackSize.x = wr.right - wr.left;
            mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        }
        return 0;
    }

    case WM_SIZE:
        if (d) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (d->render_hwnd)
                MoveWindow(d->render_hwnd, 0, 0, rc.right, rc.bottom, TRUE);
            agent_overlay_resize(d->agent_overlay);
            idd_request_render(d, TRUE);
            if (d->size_mode == DISPLAY_SIZE_WINDOW) idd_update_display_size(d, FALSE);
            if (d->relative_mouse) {
                AcquireSRWLockExclusive(&g_mouse_capture_lock);
                if (g_mouse_capture_hwnd == hwnd) idd_clip_mouse(d);
                ReleaseSRWLockExclusive(&g_mouse_capture_lock);
            }
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_MOVE:
        if (d) agent_overlay_resize(d->agent_overlay);
        if (d && d->size_mode == DISPLAY_SIZE_WINDOW && !d->input_sizing)
            idd_update_display_size(d, FALSE);
        if (d && d->relative_mouse) {
            AcquireSRWLockExclusive(&g_mouse_capture_lock);
            if (g_mouse_capture_hwnd == hwnd) idd_clip_mouse(d);
            ReleaseSRWLockExclusive(&g_mouse_capture_lock);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        if (d) {
            d->input_sizing = msg == WM_ENTERSIZEMOVE;
            if (d->size_mode == DISPLAY_SIZE_WINDOW && d->input_sizing &&
                TryEnterCriticalSection(&d->frame_cs)) {
                ZeroMemory(&d->size_request, sizeof(d->size_request));
                LeaveCriticalSection(&d->frame_cs);
            } else if (!d->input_sizing &&
                (d->size_mode == DISPLAY_SIZE_WINDOW || d->size_follow_pending))
                idd_update_display_size(d, FALSE);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_IDD_DISPLAY_SIZE:
        if (d && lp) d->size_mode = DISPLAY_SIZE_GUEST;
        if (d && (wp || d->shared_refresh || d->size_mode == DISPLAY_SIZE_WINDOW || d->guest_size_mode != DISPLAY_SIZE_GUEST))
            idd_update_display_size(d, wp != 0);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (d) idd_request_render(d, FALSE);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_PRESENT && d) {
            if (d->size_update_pending && !d->input_sizing && !IsIconic(hwnd))
                idd_update_display_size(d, FALSE);
            idd_poll_mouse_position(d);
            {
                UINT size = (UINT)InterlockedCompareExchange(&d->rendered_size, 0, 0);
                agent_overlay_update(d->agent_overlay, d->agent_control, d->agent_x, d->agent_y,
                    LOWORD(size), HIWORD(size), d->agent_waiting, d->agent_finished);
            }
            if ((!d->shared_ready && d->frame_dirty) || !d->device ||
                (!d->rtv && !IsIconic(hwnd)))
                idd_request_render(d, FALSE);
        }
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) idd_request_render(d, FALSE);
        return 0;

    case WM_IDD_TITLE_CHANGED:
        if (d) {
            wchar_t title[300];
            UINT size = (UINT)InterlockedCompareExchange(&d->rendered_size, 0, 0);
            swprintf_s(title, ARRAYSIZE(title), L"%s%s Display %ux%u",
                d->audio_muted ? L"\U0001F507 " : L"",
                d->vm_name, (UINT)LOWORD(size), (UINT)HIWORD(size));
            SetWindowTextW(hwnd, title);
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (d && d->clipboard) {
            vm_clipboard_on_clipboard_update(d->clipboard);
        }
        return 0;

    case WM_CLIP_READER_APPLY:
        if (d && d->clipboard) {
            vm_clipboard_on_reader_apply(d->clipboard);
        }
        return 0;

    /* Posted by vm_display_idd_focus() from another thread to raise an
       already-open window. Runs on the window's own thread. Restores from
       minimized (the creation path never had to handle that) then brings
       the window forward; SetForegroundWindow succeeds because the user
       just clicked our foreground main window to trigger this. */
    case WM_IDD_FOCUS:
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        return 0;

    case WM_IDD_INPUT_READY:
        if (d && !d->stop) {
            idd_cancel_mouse_sync(d);
            idd_update_kbd_hook(d);
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_IDD_CURSOR_CHANGED:
        if (d && d->render_hwnd) {
            idd_update_relative_mouse(d);
            POINT pt;
            if (GetCursorPos(&pt) && WindowFromPoint(pt) == d->render_hwnd)
                SendMessageW(d->render_hwnd, WM_SETCURSOR,
                             (WPARAM)d->render_hwnd,
                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
        return 0;

    case WM_ENTERMENULOOP:
        if (d) {
            d->input_menu_active = TRUE;
            if (d->keyboard_version == INPUT_KEYBOARD_VERSION) idd_flush_held_keys(d);
            idd_flush_mouse_buttons(d);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_EXITMENULOOP:
        if (d) {
            d->input_menu_active = FALSE;
            idd_update_relative_mouse(d);
        }
        break;

    case WM_SETFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, TRUE);
        break;

    case WM_KILLFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, FALSE);
        break;

    /* Top-level activation gates keyboard forwarding. Tracking activation
       (not WM_KILLFOCUS) is correct because focus moves between this window
       and its render child without losing activation. Losing activation —
       e.g. the user clicks another window or Alt+Tabs away with a key held —
       flushes held keys so nothing sticks down in the guest. */
    case WM_ACTIVATE:
        if (d) {
            d->input_focused = (LOWORD(wp) != WA_INACTIVE);
            if (!d->input_focused) {
                agent_overlay_cancel_input(d->agent_overlay);
                idd_flush_held_keys(d);
                idd_flush_mouse_buttons(d);
            }
            idd_update_relative_mouse(d);
        }
        break;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
    case WM_INPUT:
        if (d && d->relative_mouse && d->input_focused &&
            d->frame_connected && !d->cursor_visible &&
            d->mouse_version >= INPUT_MOUSE_VERSION &&
            GetForegroundWindow() == hwnd) {
            RAWINPUT raw = {0};
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                raw.header.dwType == RIM_TYPEMOUSE) {
                LONG dx = raw.data.mouse.lLastX, dy = raw.data.mouse.lLastY;
                if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    BOOL virtual_desktop = (raw.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                    POINT pt = {
                        MulDiv(dx, GetSystemMetrics(virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535),
                        MulDiv(dy, GetSystemMetrics(virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535)
                    };
                    dx = dy = 0;
                    if (d->raw_absolute_valid && d->raw_absolute_device == raw.header.hDevice) {
                        dx = pt.x - d->raw_absolute_position.x;
                        dy = pt.y - d->raw_absolute_position.y;
                    }
                    d->raw_absolute_valid = TRUE;
                    d->raw_absolute_device = raw.header.hDevice;
                    d->raw_absolute_position = pt;
                } else {
                    d->raw_absolute_valid = FALSE;
                }
                if (dx || dy) {
                    send_input(d, INPUT_MOUSE_RELATIVE, (UINT32)dx, (UINT32)dy, 0);
                }
            }
        }
        break;

    case WM_MOUSEMOVE:
        if (d) {
            if (!d->tracking && d->render_hwnd) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = d->render_hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                d->tracking = TRUE;
            }
            d->mouse_in = TRUE;
            if (!d->relative_mouse && !d->cursor_visible)
                idd_update_relative_mouse(d);

            if (!d->relative_mouse && !d->mouse_sync_pending) {
                UINT size = (UINT)InterlockedCompareExchange(&d->rendered_size, 0, 0);
                send_mouse_move(d, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp), size);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (d) {
            d->mouse_in = FALSE;
            d->tracking = FALSE;
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT point;
            BOOL menu = d && GetCursorPos(&point) && ScreenToClient(d->render_hwnd, &point) &&
                agent_overlay_hit_test(d->agent_overlay, point);
            if (menu)
                SetCursor(LoadCursorW(NULL, IDC_HAND));
            else if (d && d->agent_control == ASB_CONTROL_AGENT)
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            else if (d && (!d->cursor_visible || d->mouse_sync_pending))
                SetCursor(NULL);
            else if (d && d->guest_cursor)
                SetCursor(d->guest_cursor);
            else
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    /* ---- Mouse button/wheel forwarding (only when cursor is in render area) ---- */
    case WM_LBUTTONDOWN:
        if (d && d->mouse_in) {
            if (d->render_hwnd) SetCapture(d->render_hwnd);
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_LEFT;
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_LEFT)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_LEFT);
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (d && d->mouse_in) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_RIGHT;
        }
        return 0;
    case WM_RBUTTONUP:
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_RIGHT)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_RIGHT);
        }
        return 0;

    case WM_MBUTTONDOWN:
        if (d && d->mouse_in) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_MIDDLE;
        }
        return 0;
    case WM_MBUTTONUP:
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_MIDDLE)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_MIDDLE);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;

    /* ---- Keyboard input forwarding (gated on window activation) ----
       In Transmit mode key events are captured by the low-level hook
       and never reach here. The mode check below covers Default mode: a
       reserved hotkey is neither forwarded to the guest nor consumed — it
       falls through to DefWindowProc so the host handles it normally (this
       is what avoids the old stuck-key bug). Normal keys are forwarded while
       the window is the active foreground window. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        BOOL ext = (lp & (1 << 24)) != 0;
        BOOL up  = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
        if (!d) break;
        if (!d->transmit_hotkeys &&
            idd_is_reserved_hotkey((DWORD)wp, (GetKeyState(VK_MENU) & 0x8000) != 0))
            break;  /* Default mode: let the host handle this hotkey. */
        if (d->kbd_hook && d->keyboard_version == INPUT_KEYBOARD_VERSION)
            return 0;  /* The hook already forwarded modifiers passed through to the host. */
        if (d->input_focused)
            idd_forward_key(d, (DWORD)wp, scan, ext, up);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Public API
 * ================================================================== */

static volatile LONG64 g_next_display_token;
UINT_PTR vm_display_idd_token(VmDisplayIdd *display) { return display ? display->token : 0; }

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    VmDisplayIdd *d;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm_id        = vm->unique_id;
    d->token        = ((UINT_PTR)InterlockedIncrement64(&g_next_display_token) << 1) | 1;
    wcscpy_s(d->vm_name, 256, vm->name);
    wcscpy_s(d->vhdx_path, MAX_PATH, vm->vhdx_path);
    d->runtime_id   = vm->runtime_id;
    wcscpy_s(d->os_type, 32, vm->os_type);
    wcscpy_s(d->gpu_id, 512, vm->gpu_id);
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->open         = TRUE;
    d->stop         = FALSE;
    InitializeSRWLock(&d->input_lock);
    d->keyboard_version   = 1;
    d->input_socket       = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;
    d->frame_socket       = INVALID_SOCKET;
    d->shared_front       = -1;
    d->shared_pending     = -1;
    d->clipboard          = NULL;
    d->cursor_visible     = TRUE;
    d->agent_control = d->agent_x = d->agent_y = -1;
    d->agent_finished = TRUE;

    /* Load the per-VM display setting, creating display_settings.json with
       the default (off) if this VM doesn't have one yet. The hook itself is
       installed later, on the window thread, once the window exists. */
    d->transmit_hotkeys = idd_display_settings_load_or_create(vm->vhdx_path);

    /* Initialize frame buffer at default resolution */
    d->frame_width  = DEFAULT_WIDTH;
    d->frame_height = DEFAULT_HEIGHT;
    d->frame_stride = DEFAULT_WIDTH * 4;
    d->frame_buf    = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         d->frame_stride * d->frame_height);
    if (!d->frame_buf) {
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    InitializeCriticalSection(&d->frame_cs);
    InitializeConditionVariable(&d->gpu_refresh_done);

    /* Start the window thread (which will then start the recv thread) */
    d->window_thread = CreateThread(NULL, 0, idd_window_thread_proc, d, 0, NULL);
    if (!d->window_thread) {
        ui_log(L"IDD: Failed to create window thread.");
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

void vm_display_idd_destroy(VmDisplayIdd *display)
{
    if (!display) return;

    /* Signal stop */
    display->stop = TRUE;
    display->open = FALSE;
    if (display->shared_wake) SetEvent(display->shared_wake);

    EnterCriticalSection(&display->frame_cs);
    if (display->frame_socket != INVALID_SOCKET)
        shutdown(display->frame_socket, SD_BOTH);
    if (display->audio_socket != INVALID_SOCKET)
        shutdown(display->audio_socket, SD_BOTH);
    LeaveCriticalSection(&display->frame_cs);

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        idd_join_thread(display->window_thread);
        CloseHandle(display->window_thread);
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        idd_join_thread(display->recv_thread);
        CloseHandle(display->recv_thread);
    }

    /* Clipboard is cleaned up by WM_CLOSE handler, but guard */
    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    /* audio_recv_thread guard */
    if (display->audio_recv_thread) {
        idd_join_thread(display->audio_recv_thread);
        CloseHandle(display->audio_recv_thread);
    }

    DeleteCriticalSection(&display->frame_cs);

    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    HeapFree(GetProcessHeap(), 0, display);
}

BOOL vm_display_idd_is_active(VmDisplayIdd *display)
{
    return display && display->open && !display->stop;
}

BOOL vm_display_idd_is_open(VmDisplayIdd *display)
{
    if (!display) return FALSE;
    return display->open && display->hwnd && IsWindow(display->hwnd);
}

void vm_display_idd_focus(VmDisplayIdd *display)
{
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd))
        return;
    /* Marshal to the window thread; that thread owns the window and runs
       the activation (restore-if-minimized + foreground) in WM_IDD_FOCUS. */
    PostMessageW(display->hwnd, WM_IDD_FOCUS, 0, 0);
}
