/*
 * vm_display_idd.c -- Host-side IDD frame receiver + D3D11 renderer.
 *
 * Connects to the guest VM over AF_HYPERV sockets:
 *   :0002  Frame channel — receives frames, renders via D3D11 textured quad
 *   :0003  Input channel — forwards keyboard/mouse events to guest
 *   :0005  Clipboard writer — sends host clipboard to guest (host→guest)
 *   :0006  Clipboard reader — receives guest clipboard from reader (guest→host)
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
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#pragma warning(pop)

#include <stdio.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "vm_agent.h"
#include "ui.h"
#include "resource.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#include <shellapi.h>
#include <shlobj.h>

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

/* Input channel service GUID — connects to agent for SendInput injection */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Clipboard channel service GUID — connects to guest clipboard writer (host→guest) */
static const GUID CLIPBOARD_SERVICE_GUID =
    { 0xa5b0cafe, 0x0005, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Clipboard reader channel — connects to guest clipboard reader (guest→host) */
static const GUID CLIPBOARD_READER_SERVICE_GUID =
    { 0xa5b0cafe, 0x0006, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

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

/* ---- Clipboard protocol ---- */

#define CLIP_MAGIC          0x504C4341  /* "ACLP" little-endian */
#define CLIP_READY_MAGIC    0x59444C43  /* "CLDY" little-endian */

/* Protocol: delayed rendering (like MS-RDPECLIP).
   On clipboard change, only the format list is sent.
   Actual data is fetched on demand via request/response. */
#define CLIP_MSG_FORMAT_LIST      1  /* format list (clipboard changed) */
#define CLIP_MSG_FORMAT_DATA_REQ  2  /* request data for a format */
#define CLIP_MSG_FORMAT_DATA_RESP 3  /* response with data */
#define CLIP_MSG_FILE_DATA        4  /* file content (part of CF_HDROP response) */

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)  /* 64 MB */

#define WM_CLIP_READER_APPLY (WM_APP + 11)  /* reader recv thread -> wndproc: apply reader data */

#pragma pack(push, 1)
typedef struct ClipHeader {
    UINT32 magic;
    UINT32 msg_type;
    UINT32 format;
    UINT32 data_size;
} ClipHeader;
#pragma pack(pop)

/* Format entry: maps local↔remote IDs and holds fetched data */
typedef struct {
    UINT   local_id;
    UINT   remote_id;
    BYTE  *data;
    UINT32 data_size;
    BOOL   is_hdrop;   /* TRUE = files in temp dir, build DROPFILES */
} ClipFmtEntry;

/* File transfer for CF_HDROP — sends actual file contents across VM boundary */
#define CLIP_FILE_CHUNK     (1024 * 1024)  /* 1 MB — HV socket is basically memcpy */
static wchar_t g_clip_temp_dir[MAX_PATH];

#pragma pack(push, 1)
typedef struct ClipFileInfo {
    UINT32 path_len;     /* relative path length in bytes (UTF-16LE, no null) */
    UINT64 file_size;    /* file data bytes that follow (0 for directories) */
    UINT8  is_directory; /* 1 = directory entry, 0 = regular file */
} ClipFileInfo;
#pragma pack(pop)

/* ---- Frame protocol constants ---- */

#define FRAME_MAGIC         0x52465341  /* "ASFR" little-endian */
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MAX_DIRTY_RECTS     64
#define MAX_FRAME_DATA_SIZE (DEFAULT_WIDTH * DEFAULT_HEIGHT * 4)

/* ---- Input protocol (host → guest) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3

/* Button IDs for INPUT_MOUSE_BUTTON */
#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2

#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" little-endian */

#pragma pack(push, 1)
typedef struct InputPacket {
    UINT32 magic;   /* INPUT_MAGIC */
    UINT32 type;    /* INPUT_MOUSE_MOVE / BUTTON / WHEEL / KEY */
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;
#pragma pack(pop)

/* ---- Window messages ---- */

#define WM_VM_DISPLAY_CLOSED    (WM_APP + 5)
#define WM_IDD_FRAME_READY      (WM_USER + 100)

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

struct VmDisplayIdd {
    VmInstance  *vm;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    volatile BOOL open;
    volatile BOOL stop;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    ID3D11ShaderResourceView *frame_srv;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11SamplerState      *sampler;

    /* Frame buffer (CPU-side, updated by recv thread) */
    BYTE          *frame_buf;
    UINT           frame_width;
    UINT           frame_height;
    UINT           frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL  frame_dirty;

    UINT           render_count;     /* number of renders (for one-shot logging) */
    volatile UINT  recv_count;       /* number of frames received over HvSocket */

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */

    /* Guest cursor */
    HCURSOR        guest_cursor;    /* current cursor created from guest bitmap */
    UINT32         cursor_shape_id; /* tracks which shape is current */
    BOOL           cursor_visible;  /* guest cursor visibility */

    /* Debug log window (separate top-level window) */
    HWND           log_hwnd;        /* top-level log window */
    HWND           log_list_hwnd;   /* listbox inside log window */
    HWND           render_hwnd;     /* child window for D3D11 rendering */

    /* Clipboard writer channel (:0005 — host→guest, delayed rendering) */
    volatile SOCKET  clip_socket;
    volatile LONG    clip_suppress;
    HANDLE           clip_recv_thread;
    CRITICAL_SECTION clip_cs;

    /* Format data: filled by recv thread, applied by wndproc */
    ClipFmtEntry     clip_fmts[CLIP_MAX_FORMATS];
    int              clip_fmt_count;
    int              clip_fetch_idx;  /* next format to request; == count means all fetched */

    /* Clipboard reader channel (:0006 — guest→host) */
    volatile SOCKET  clip_reader_socket;
    HANDLE           clip_reader_recv_thread;
    volatile LONG    clip_reader_suppress;
    CRITICAL_SECTION clip_reader_cs;
    ClipFmtEntry     clip_reader_fmts[CLIP_MAX_FORMATS];
    int              clip_reader_fmt_count;
    int              clip_reader_fetch_idx;

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

/* ---- Window class ---- */

static const wchar_t *IDD_DISPLAY_CLASS = L"AppSandboxIddDisplay";
static const wchar_t *IDD_RENDER_CLASS  = L"AppSandboxIddRender";
static const wchar_t *IDD_LOG_CLASS     = L"AppSandboxIddLog";

/* System menu command IDs — must be < 0xF000 and have low 4 bits clear */
#define IDM_AUDIO_MUTE  0x1000
static BOOL g_idd_class_registered;
static WNDPROC g_orig_listbox_proc;

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
    case WM_PAINT:
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

    if (!d || !d->log_list_hwnd || d->stop) return;

    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);

    count = SendMessageW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf);

    /* Trim old entries */
    while (count > MAX_LOG_LINES) {
        SendMessageW(d->log_list_hwnd, LB_DELETESTRING, 0, 0);
        count--;
    }

    /* Scroll to bottom and force repaint even when not focused */
    SendMessageW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
    UpdateWindow(d->log_list_hwnd);
}

/* ---- Send input packet to guest ---- */

static UINT g_input_send_count = 0;

static void send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    SOCKET s;
    int ret;

    s = d->input_socket;
    if (s == INVALID_SOCKET) return;

    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    /* Non-blocking send — drop packet if buffer full rather than stall the UI */
    ret = send(s, (const char *)&pkt, (int)sizeof(pkt), 0);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        idd_log(d, L"INPUT SEND ERR %d - flagging for reconnect.", err);
        /* Mark dead — recv thread owns the socket and will close + reconnect */
        d->input_socket = INVALID_SOCKET;
        return;
    }

    g_input_send_count++;

    /* Log non-move events only (moves are too noisy) */
    if (type != INPUT_MOUSE_MOVE) {
        static const wchar_t *type_names[] = {
            L"MOUSE_MOVE", L"MOUSE_BTN", L"MOUSE_WHEEL", L"KEY"
        };
        const wchar_t *name = type < 4 ? type_names[type] : L"?";
        idd_log(d, L"INPUT %s p1=%u p2=%u p3=%u (#%u)", name, p1, p2, p3, g_input_send_count);
    }
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
                                 UINT vm_w, UINT vm_h,
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

    local_x = ((float)wx - vp_x) / vp_w * (float)vm_w;
    local_y = ((float)wy - vp_y) / vp_h * (float)vm_h;

    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    *vx = (UINT)local_x;
    *vy = (UINT)local_y;
    if (*vx >= vm_w) *vx = vm_w - 1;
    if (*vy >= vm_h) *vy = vm_h - 1;
}

/* ---- Reliable recv: read exactly `len` bytes ---- */

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
    WAVEFORMATEX *mixfmt = NULL;
    WAVEFORMATEX *renderfmt = NULL;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    UINT32 buf_frames = 0;
    UINT32 render_block_align = 0;
    AudioHeader hdr;
    BOOL stream_started = FALSE;
    BOOL session_logged = FALSE;
    int  header_misses = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        com_ok = TRUE;

    /* Outer retry loop — keep trying to connect to the guest capture helper */
    while (!d->stop) {
        SOCKET s = connect_to_hv_service(&d->runtime_id, &AUDIO_SERVICE_GUID, 1000);
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
        d->audio_socket = s;

        /* Read one-shot AudioHeader.
           The guest closes without sending a header when the VAD endpoint
           isn't ready yet (e.g. before user login / audio stack init).
           Log only the first miss per run so we don't spam. */
        if (!recv_exact(s, &hdr, sizeof(hdr)) || hdr.magic != AUDIO_HEADER_MAGIC) {
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

        hr = IAudioClient_GetMixFormat(pAC, &mixfmt);
        if (FAILED(hr) || !mixfmt) { idd_log(d, L"Audio: GetMixFormat failed 0x%08lX.", hr); goto session_cleanup; }

        /* Build the source format exactly matching what the guest is sending. */
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

        /* 40ms buffer. HV socket is effectively memcpy and the guest polls
           every 5ms, so scheduler jitter is the only thing we need to absorb. */
        hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000, 0, renderfmt, NULL);
        if (FAILED(hr)) {
            idd_log(d, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr);
            goto session_cleanup;
        }

        hr = IAudioClient_GetBufferSize(pAC, &buf_frames);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioRenderClient, (void **)&pRC);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_Start(pAC);
        if (FAILED(hr)) goto session_cleanup;
        stream_started = TRUE;

        idd_log(d, L"Audio render started: buf=%lu frames.", buf_frames);

        /* Receive loop */
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 padding;
            UINT32 free_frames;
            UINT32 frames_to_write;
            BYTE *dst;

            if (!recv_exact(s, &fh, sizeof(fh))) break;
            if (fh.bytes == 0 || fh.bytes > 4 * 1024 * 1024) break;

            if (fh.bytes > scratch_cap) {
                BYTE *nb = scratch
                    ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, fh.bytes)
                    : (BYTE *)HeapAlloc(GetProcessHeap(), 0, fh.bytes);
                if (!nb) break;
                scratch = nb;
                scratch_cap = fh.bytes;
            }
            if (!recv_exact(s, scratch, (int)fh.bytes)) break;

            /* Host-side mute: keep draining the socket but don't push to WASAPI */
            if (d->audio_muted) continue;

            frames_in_payload = fh.bytes / render_block_align;
            if (frames_in_payload == 0) continue;

            /* Push into WASAPI buffer; drop if no room (low-latency, no stalling) */
            hr = IAudioClient_GetCurrentPadding(pAC, &padding);
            if (FAILED(hr)) break;
            free_frames = buf_frames - padding;
            frames_to_write = frames_in_payload;
            if (frames_to_write > free_frames)
                frames_to_write = free_frames;
            if (frames_to_write == 0)
                continue;

            hr = IAudioRenderClient_GetBuffer(pRC, frames_to_write, &dst);
            if (FAILED(hr)) break;
            memcpy(dst, scratch, (size_t)frames_to_write * render_block_align);
            IAudioRenderClient_ReleaseBuffer(pRC, frames_to_write, 0);
        }

session_cleanup:
        if (stream_started && pAC) {
            IAudioClient_Stop(pAC);
            stream_started = FALSE;
        }
        if (pRC)    { IAudioRenderClient_Release(pRC);    pRC = NULL; }
        if (pAC)    { IAudioClient_Release(pAC);          pAC = NULL; }
        if (pDev)   { IMMDevice_Release(pDev);            pDev = NULL; }
        if (pEnum)  { IMMDeviceEnumerator_Release(pEnum); pEnum = NULL; }
        if (mixfmt)    { CoTaskMemFree(mixfmt);    mixfmt = NULL; }
        if (renderfmt) { CoTaskMemFree(renderfmt); renderfmt = NULL; }

        if (d->audio_socket != INVALID_SOCKET) {
            closesocket(d->audio_socket);
            d->audio_socket = INVALID_SOCKET;
        }
        if (session_logged) {
            idd_log(d, L"Audio session ended.");
            session_logged = FALSE;
        }

        if (d->stop) break;
        /* Back off when the guest keeps rejecting us (VAD not ready) */
        Sleep(header_misses > 3 ? 5000 : 500);
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (com_ok)  CoUninitialize();
    d->audio_recv_thread = NULL;
    return 0;
}

/* ==================================================================
 * Clipboard sync — host ↔ guest
 * ================================================================== */

/* Skip non-transferable clipboard formats (GDI handles, owner-display, etc.)
   CF_HDROP IS transferable via the file-data sub-protocol. */
static BOOL clip_should_skip(UINT fmt)
{
    switch (fmt) {
    case 0:
    case CF_BITMAP:            /* GDI handle — use CF_DIB instead */
    case CF_PALETTE:           /* GDI handle */
    case CF_OWNERDISPLAY:
    case CF_METAFILEPICT:      /* GDI handle */
    case CF_ENHMETAFILE:       /* GDI handle */
    case CF_DSPTEXT:
    case CF_DSPBITMAP:
    case CF_DSPMETAFILEPICT:
    case CF_DSPENHMETAFILE:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL clip_send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = send(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

static void clip_init_temp_dir(void)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_clip_temp_dir, MAX_PATH, L"%sAppSandboxClip", tmp);
    CreateDirectoryW(g_clip_temp_dir, NULL);
}

/* ---- File transfer helpers ---- */

static void clip_remove_dir_recursive(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t path[MAX_PATH];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            clip_remove_dir_recursive(path);
        else
            DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
}

static void clip_ensure_parent_dir(const wchar_t *file_path)
{
    wchar_t dir[MAX_PATH];
    wchar_t *p;
    wcscpy_s(dir, MAX_PATH, file_path);
    p = wcsrchr(dir, L'\\');
    if (!p) return;
    *p = L'\0';
    for (p = dir; *p; p++) {
        if (*p == L'\\' && p > dir && *(p - 1) != L':') {
            *p = L'\0';
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(dir, NULL);
}

static BOOL clip_path_is_safe(const wchar_t *rel_path)
{
    if (wcsstr(rel_path, L"..")) return FALSE;
    if (rel_path[0] == L'\\' || rel_path[0] == L'/') return FALSE;
    if (wcslen(rel_path) > MAX_PATH - 50) return FALSE;
    return TRUE;
}

/* Send one file or directory (recursive) as CLIP_MSG_FILE_DATA messages */
static BOOL clip_send_file_entry(SOCKET s, const wchar_t *full_path,
                                  const wchar_t *rel_path)
{
    ClipHeader hdr;
    ClipFileInfo fi;
    DWORD attrs = GetFileAttributesW(full_path);
    UINT32 path_bytes;

    if (attrs == INVALID_FILE_ATTRIBUTES) return TRUE; /* skip missing */

    path_bytes = (UINT32)(wcslen(rel_path) * sizeof(wchar_t));
    fi.path_len = path_bytes;

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        wchar_t search[MAX_PATH];
        HANDLE h;

        fi.file_size = 0;
        fi.is_directory = 1;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!clip_send_all(s, &hdr, sizeof(hdr))) return FALSE;
        if (!clip_send_all(s, &fi, sizeof(fi))) return FALSE;
        if (path_bytes > 0 && !clip_send_all(s, rel_path, (int)path_bytes)) return FALSE;

        /* Recurse into directory */
        swprintf_s(search, MAX_PATH, L"%s\\*", full_path);
        h = FindFirstFileW(search, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t child_full[MAX_PATH], child_rel[MAX_PATH];
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                swprintf_s(child_full, MAX_PATH, L"%s\\%s", full_path, fd.cFileName);
                swprintf_s(child_rel, MAX_PATH, L"%s\\%s", rel_path, fd.cFileName);
                if (!clip_send_file_entry(s, child_full, child_rel)) {
                    FindClose(h);
                    return FALSE;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        /* Regular file — stream contents */
        HANDLE hFile = CreateFileW(full_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER size;
        BYTE *chunk;
        UINT64 remaining;

        if (hFile == INVALID_HANDLE_VALUE) return TRUE; /* skip unreadable */

        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
        if (!chunk) { CloseHandle(hFile); return FALSE; }

        GetFileSizeEx(hFile, &size);
        fi.file_size = (UINT64)size.QuadPart;
        fi.is_directory = 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!clip_send_all(s, &hdr, sizeof(hdr)) ||
            !clip_send_all(s, &fi, sizeof(fi)) ||
            (path_bytes > 0 && !clip_send_all(s, rel_path, (int)path_bytes))) {
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
            return FALSE;
        }

        remaining = fi.file_size;
        while (remaining > 0) {
            DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
            DWORD bytes_read;
            if (!ReadFile(hFile, chunk, to_read, &bytes_read, NULL) || bytes_read == 0) {
                HeapFree(GetProcessHeap(), 0, chunk);
                CloseHandle(hFile);
                return FALSE;
            }
            if (!clip_send_all(s, chunk, (int)bytes_read)) {
                HeapFree(GetProcessHeap(), 0, chunk);
                CloseHandle(hFile);
                return FALSE;
            }
            remaining -= bytes_read;
        }
        HeapFree(GetProcessHeap(), 0, chunk);
        CloseHandle(hFile);
    }
    return TRUE;
}

/* Build CF_HDROP (DROPFILES) from top-level items in temp dir */
static HGLOBAL clip_build_hdrop_from_temp(void)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
    int count = 0;
    SIZE_T total_size;
    HGLOBAL hMem;
    DROPFILES *df;
    wchar_t *ptr;
    int i;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", g_clip_temp_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(paths[count], MAX_PATH, L"%s\\%s", g_clip_temp_dir, fd.cFileName);
        count++;
        if (count >= CLIP_MAX_FORMATS) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (count == 0) return NULL;

    /* DROPFILES + null-terminated wide strings + double null */
    total_size = sizeof(DROPFILES);
    for (i = 0; i < count; i++)
        total_size += (wcslen(paths[i]) + 1) * sizeof(wchar_t);
    total_size += sizeof(wchar_t);

    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_size);
    if (!hMem) return NULL;

    df = (DROPFILES *)GlobalLock(hMem);
    if (!df) { GlobalFree(hMem); return NULL; }

    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;

    ptr = (wchar_t *)((BYTE *)df + sizeof(DROPFILES));
    for (i = 0; i < count; i++) {
        wcscpy_s(ptr, MAX_PATH, paths[i]);
        ptr += wcslen(paths[i]) + 1;
    }
    *ptr = L'\0';

    GlobalUnlock(hMem);
    return hMem;
}

/* Send format list to guest (clipboard changed locally).
   Only sends format IDs + names — no data yet (delayed rendering). */
static void clip_send_format_list(VmDisplayIdd *d)
{
    ClipHeader hdr;
    SOCKET s;
    UINT fmt;
    BYTE buf[16384];
    int offset = 4;  /* skip count field */
    UINT32 count = 0;

    s = d->clip_socket;
    if (s == INVALID_SOCKET) {
        idd_log(d, L"CLIP: clip_send_format_list skipped — no socket.");
        return;
    }

    if (!OpenClipboard(d->hwnd)) {
        idd_log(d, L"CLIP: OpenClipboard failed (%lu).", GetLastError());
        return;
    }

    /* Check if CF_HDROP is present — if so, only send CF_HDROP.
       Other shell formats (Shell IDList Array, FileNameW, etc.) contain
       host-local paths/PIDLs that are meaningless on the remote side. */
    {
        BOOL has_hdrop = IsClipboardFormatAvailable(CF_HDROP);

        fmt = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0 && count < CLIP_MAX_FORMATS) {
            char name[256];
            int name_len = 0;

            if (clip_should_skip(fmt)) continue;

            /* When files are on the clipboard, only transfer CF_HDROP —
               the file-data sub-protocol will deliver actual contents. */
            if (has_hdrop && fmt != CF_HDROP) continue;

            if (fmt >= 0xC000)
                name_len = GetClipboardFormatNameA(fmt, name, sizeof(name));

            /* Bounds check */
            if (offset + 8 + name_len > 16384) break;

            *(UINT32 *)(buf + offset) = fmt;       offset += 4;
            *(UINT32 *)(buf + offset) = (UINT32)name_len; offset += 4;
            if (name_len > 0) {
                memcpy(buf + offset, name, name_len);
                offset += name_len;
            }
            count++;
        }
    }
    CloseClipboard();

    *(UINT32 *)buf = count;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_LIST;
    hdr.format = 0;
    hdr.data_size = (UINT32)offset;

    /* Suppress the echo: guest writer will apply this, triggering
       WM_CLIPBOARDUPDATE in the guest reader which sends it back on :0006.
       Tell the reader recv thread to ignore the next FORMAT_LIST. */
    InterlockedExchange(&d->clip_reader_suppress, 1);

    EnterCriticalSection(&d->clip_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    clip_send_all(s, buf, offset);
    LeaveCriticalSection(&d->clip_cs);

    idd_log(d, L"CLIP: Sent format list (%u formats).", count);
}

/* Handle FORMAT_DATA_REQ from guest: read local clipboard and send response.
   Called from the recv thread. */
static void clip_handle_data_request(VmDisplayIdd *d, UINT fmt)
{
    ClipHeader hdr;
    SOCKET s = d->clip_socket;

    if (s == INVALID_SOCKET) return;

    /* CF_HDROP: send file contents, then empty response */
    if (fmt == CF_HDROP) {
        if (OpenClipboard(NULL)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)hData;
                UINT file_count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                UINT fi;
                idd_log(d, L"CLIP: Data request CF_HDROP — %u files.", file_count);
                EnterCriticalSection(&d->clip_cs);
                for (fi = 0; fi < file_count; fi++) {
                    wchar_t path[MAX_PATH];
                    wchar_t *name;
                    DragQueryFileW(hDrop, fi, path, MAX_PATH);
                    name = wcsrchr(path, L'\\');
                    name = name ? name + 1 : path;
                    clip_send_file_entry(s, path, name);
                }
                /* Signal end with empty response */
                hdr.magic = CLIP_MAGIC;
                hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
                hdr.format = CF_HDROP;
                hdr.data_size = 0;
                clip_send_all(s, &hdr, sizeof(hdr));
                LeaveCriticalSection(&d->clip_cs);
            } else {
                idd_log(d, L"CLIP: GetClipboardData(CF_HDROP) returned NULL.");
                goto send_empty;
            }
            CloseClipboard();
        } else {
            idd_log(d, L"CLIP: OpenClipboard failed for CF_HDROP (%lu).", GetLastError());
            goto send_empty;
        }
        return;

    send_empty:
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&d->clip_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        LeaveCriticalSection(&d->clip_cs);
        return;
    }

    /* Regular format: read and send */
    if (OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(fmt);
        void *ptr = hData ? GlobalLock(hData) : NULL;
        SIZE_T size = ptr ? GlobalSize(hData) : 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = (ptr && size <= CLIP_MAX_PAYLOAD) ? (UINT32)size : 0;

        EnterCriticalSection(&d->clip_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        if (hdr.data_size > 0)
            clip_send_all(s, ptr, (int)hdr.data_size);
        LeaveCriticalSection(&d->clip_cs);

        if (ptr) GlobalUnlock(hData);
        CloseClipboard();
        idd_log(d, L"CLIP: Responded format %u (%u bytes).", fmt, hdr.data_size);
    } else {
        /* Can't open clipboard — send empty response */
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&d->clip_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        LeaveCriticalSection(&d->clip_cs);
    }
}

/* Free all pending format data */
static void clip_free_pending(VmDisplayIdd *d)
{
    int i;
    for (i = 0; i < d->clip_fmt_count; i++) {
        if (d->clip_fmts[i].data) {
            HeapFree(GetProcessHeap(), 0, d->clip_fmts[i].data);
            d->clip_fmts[i].data = NULL;
        }
    }
}

/* Handle a single clipboard protocol message. Returns TRUE to continue, FALSE on error. */
static BOOL clip_handle_message(VmDisplayIdd *d, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {

    case CLIP_MSG_FORMAT_DATA_REQ:
        idd_log(d, L"CLIP: Data request for format %u.", hdr->format);
        clip_handle_data_request(d, hdr->format);
        return TRUE;

    default:
        idd_log(d, L"CLIP: Unknown msg_type %u.", hdr->msg_type);
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

/* Clipboard recv thread — delayed rendering protocol with auto-reconnect */
static DWORD WINAPI clip_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    SOCKET s;
    ClipHeader hdr;

    idd_log(d, L"CLIP: Recv thread started.");

    /* Outer loop: connection cycle (initial + reconnects) */
    for (;;) {
        s = d->clip_socket;

        /* Message loop */
        while (!d->stop) {
            if (!recv_exact(s, &hdr, sizeof(hdr)))
                break;
            if (hdr.magic != CLIP_MAGIC) {
                idd_log(d, L"CLIP: Bad magic 0x%08X.", hdr.magic);
                break;
            }
            if (!clip_handle_message(d, s, &hdr))
                break;
        }

        /* Cleanup after disconnect */
        clip_free_pending(d);
        clip_remove_dir_recursive(g_clip_temp_dir);
        closesocket(d->clip_socket);
        d->clip_socket = INVALID_SOCKET;

        if (d->stop) break;
        idd_log(d, L"CLIP: Disconnected, will retry...");

        /* Reconnect loop */
        while (!d->stop) {
            int wait;
            SOCKET clip_s;
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            if (d->stop) break;

            clip_s = connect_to_hv_service(&d->runtime_id, &CLIPBOARD_SERVICE_GUID, 2000);
            if (clip_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(clip_s, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(clip_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    d->clip_socket = clip_s;
                    idd_log(d, L"CLIP: Reconnected (GUID :0005).");
                    break;  /* back to outer for loop → message loop */
                }
                closesocket(clip_s);
            }
        }

        if (d->stop) break;
    }

    d->clip_recv_thread = NULL;
    idd_log(d, L"CLIP: Recv thread exiting.");
    return 0;
}

/* ==================================================================
 * Clipboard reader channel (:0006 — guest→host)
 * ================================================================== */

/* Free reader pending format data */
static void clip_reader_free_pending(VmDisplayIdd *d)
{
    int i;
    for (i = 0; i < d->clip_reader_fmt_count; i++) {
        if (d->clip_reader_fmts[i].data) {
            HeapFree(GetProcessHeap(), 0, d->clip_reader_fmts[i].data);
            d->clip_reader_fmts[i].data = NULL;
        }
    }
}

/* Send FORMAT_DATA_REQ for the next reader format, or post WM_CLIP_READER_APPLY if done. */
static void clip_reader_request_next_format(VmDisplayIdd *d, SOCKET s)
{
    ClipHeader hdr;

    if (d->clip_reader_fetch_idx >= d->clip_reader_fmt_count) {
        idd_log(d, L"CLIP-R: All %d formats fetched, applying.", d->clip_reader_fmt_count);
        if (d->hwnd && IsWindow(d->hwnd))
            PostMessageW(d->hwnd, WM_CLIP_READER_APPLY, 0, 0);
        return;
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
    hdr.format = d->clip_reader_fmts[d->clip_reader_fetch_idx].remote_id;
    hdr.data_size = 0;

    EnterCriticalSection(&d->clip_reader_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    LeaveCriticalSection(&d->clip_reader_cs);

    idd_log(d, L"CLIP-R: Requesting format %u (remote=%u) [%d/%d].",
            d->clip_reader_fmts[d->clip_reader_fetch_idx].local_id,
            d->clip_reader_fmts[d->clip_reader_fetch_idx].remote_id,
            d->clip_reader_fetch_idx + 1, d->clip_reader_fmt_count);
}

/* Apply reader-fetched format data to the host clipboard. */
static void clip_reader_apply_format_list(VmDisplayIdd *d)
{
    int i;

    EnterCriticalSection(&d->clip_reader_cs);
    if (d->clip_reader_fmt_count == 0) {
        LeaveCriticalSection(&d->clip_reader_cs);
        return;
    }

    InterlockedExchange(&d->clip_suppress, 1);

    if (!OpenClipboard(d->hwnd)) {
        idd_log(d, L"CLIP-R: OpenClipboard failed (%lu).", GetLastError());
        InterlockedExchange(&d->clip_suppress, 0);
        clip_reader_free_pending(d);
        LeaveCriticalSection(&d->clip_reader_cs);
        return;
    }

    EmptyClipboard();

    for (i = 0; i < d->clip_reader_fmt_count; i++) {
        if (d->clip_reader_fmts[i].is_hdrop) {
            HGLOBAL hDrop = clip_build_hdrop_from_temp();
            if (hDrop)
                SetClipboardData(d->clip_reader_fmts[i].local_id, hDrop);
        } else if (d->clip_reader_fmts[i].data && d->clip_reader_fmts[i].data_size > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, d->clip_reader_fmts[i].data_size);
            if (hMem) {
                void *ptr = GlobalLock(hMem);
                if (ptr) {
                    memcpy(ptr, d->clip_reader_fmts[i].data, d->clip_reader_fmts[i].data_size);
                    GlobalUnlock(hMem);
                    SetClipboardData(d->clip_reader_fmts[i].local_id, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
        }
    }

    CloseClipboard();
    InterlockedExchange(&d->clip_suppress, 0);
    idd_log(d, L"CLIP-R: Applied %d formats to clipboard.", d->clip_reader_fmt_count);
    clip_reader_free_pending(d);
    LeaveCriticalSection(&d->clip_reader_cs);
}

/* Receive and write a single FILE_DATA message from the reader channel. */
static BOOL clip_reader_recv_file_data(VmDisplayIdd *d, SOCKET s, const ClipHeader *hdr)
{
    ClipFileInfo fi;
    wchar_t rel_path[MAX_PATH], full_path[MAX_PATH];

    if (hdr->data_size < sizeof(fi)) return FALSE;
    if (!recv_exact(s, &fi, sizeof(fi))) return FALSE;

    if (fi.path_len == 0 || fi.path_len > (MAX_PATH - 1) * sizeof(wchar_t)) {
        UINT32 skip_hdr = hdr->data_size - (UINT32)sizeof(fi);
        BYTE skip[4096];
        while (skip_hdr > 0) {
            int chunk = skip_hdr > 4096 ? 4096 : (int)skip_hdr;
            if (!recv_exact(s, skip, chunk)) return FALSE;
            skip_hdr -= chunk;
        }
        { UINT64 rem = fi.file_size;
          while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!recv_exact(s, skip, c)) return FALSE; rem -= c; } }
        return TRUE;
    }

    if (!recv_exact(s, rel_path, (int)fi.path_len)) return FALSE;
    rel_path[fi.path_len / sizeof(wchar_t)] = L'\0';

    if (!clip_path_is_safe(rel_path)) {
        BYTE skip[4096];
        UINT64 rem = fi.file_size;
        idd_log(d, L"CLIP-R: Rejected unsafe path.");
        while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!recv_exact(s, skip, c)) return FALSE; rem -= c; }
        return TRUE;
    }

    swprintf_s(full_path, MAX_PATH, L"%s\\%s", g_clip_temp_dir, rel_path);

    if (fi.is_directory) {
        clip_ensure_parent_dir(full_path);
        CreateDirectoryW(full_path, NULL);
    } else {
        HANDLE hFile;
        clip_ensure_parent_dir(full_path);
        hFile = CreateFileW(full_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            UINT64 remaining = fi.file_size;
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            if (!chunk) { CloseHandle(hFile); return FALSE; }
            while (remaining > 0) {
                DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
                DWORD written;
                if (!recv_exact(s, chunk, (int)to_read)) { HeapFree(GetProcessHeap(), 0, chunk); CloseHandle(hFile); return FALSE; }
                WriteFile(hFile, chunk, to_read, &written, NULL);
                remaining -= to_read;
            }
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
            idd_log(d, L"CLIP-R: File recv: %s (%llu bytes)", rel_path, fi.file_size);
        } else {
            UINT64 remaining = fi.file_size;
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            idd_log(d, L"CLIP-R: Failed to create %s (%lu)", full_path, GetLastError());
            if (!chunk) return FALSE;
            while (remaining > 0) {
                int c = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (int)remaining;
                if (!recv_exact(s, chunk, c)) { HeapFree(GetProcessHeap(), 0, chunk); return FALSE; }
                remaining -= c;
            }
            HeapFree(GetProcessHeap(), 0, chunk);
        }
    }
    return TRUE;
}

/* Handle a single message from the reader channel (:0006). */
static BOOL clip_reader_handle_message(VmDisplayIdd *d, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {

    case CLIP_MSG_FORMAT_LIST: {
        BYTE *buf;
        UINT32 count, i;
        int off;

        if (hdr->data_size > 16384) return FALSE;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
        if (!buf) return FALSE;
        if (!recv_exact(s, buf, (int)hdr->data_size)) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }

        /* Echo suppression: host just sent FORMAT_LIST on :0005, guest writer
           applied it, guest reader echoed it back.  Discard. */
        if (InterlockedExchange(&d->clip_reader_suppress, 0)) {
            idd_log(d, L"CLIP-R: FORMAT_LIST suppressed (echo from host->guest).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        count = *(UINT32 *)buf;
        if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
        off = 4;

        EnterCriticalSection(&d->clip_reader_cs);
        clip_reader_free_pending(d);
        d->clip_reader_fmt_count = 0;
        for (i = 0; i < count && off + 8 <= (int)hdr->data_size; i++) {
            UINT32 fmt_id   = *(UINT32 *)(buf + off); off += 4;
            UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
            UINT local_id = fmt_id;

            if (name_len > 0 && name_len < 256 && off + (int)name_len <= (int)hdr->data_size) {
                char name[256];
                memcpy(name, buf + off, name_len);
                name[name_len] = '\0';
                local_id = RegisterClipboardFormatA(name);
                idd_log(d, L"CLIP-R: Format '%S' remote=%u local=%u.", name, fmt_id, local_id);
            }
            off += (int)name_len;

            d->clip_reader_fmts[d->clip_reader_fmt_count].local_id = local_id;
            d->clip_reader_fmts[d->clip_reader_fmt_count].remote_id = fmt_id;
            d->clip_reader_fmts[d->clip_reader_fmt_count].data = NULL;
            d->clip_reader_fmts[d->clip_reader_fmt_count].data_size = 0;
            d->clip_reader_fmts[d->clip_reader_fmt_count].is_hdrop = FALSE;
            d->clip_reader_fmt_count++;
        }
        d->clip_reader_fetch_idx = 0;
        LeaveCriticalSection(&d->clip_reader_cs);

        HeapFree(GetProcessHeap(), 0, buf);

        idd_log(d, L"CLIP-R: Received format list (%u formats), fetching data...", d->clip_reader_fmt_count);

        clip_remove_dir_recursive(g_clip_temp_dir);
        CreateDirectoryW(g_clip_temp_dir, NULL);

        clip_reader_request_next_format(d, s);
        return TRUE;
    }

    case CLIP_MSG_FORMAT_DATA_RESP: {
        int idx = d->clip_reader_fetch_idx;
        idd_log(d, L"CLIP-R: Data response for format %u (%u bytes).", hdr->format, hdr->data_size);

        if (idx >= 0 && idx < d->clip_reader_fmt_count) {
            if (hdr->format == CF_HDROP && hdr->data_size == 0) {
                d->clip_reader_fmts[idx].is_hdrop = TRUE;
            } else if (hdr->data_size > 0 && hdr->data_size <= CLIP_MAX_PAYLOAD) {
                BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
                if (data && recv_exact(s, data, (int)hdr->data_size)) {
                    d->clip_reader_fmts[idx].data = data;
                    d->clip_reader_fmts[idx].data_size = hdr->data_size;
                } else {
                    if (data) HeapFree(GetProcessHeap(), 0, data);
                    return FALSE;
                }
            }
            d->clip_reader_fetch_idx++;
            clip_reader_request_next_format(d, s);
        }
        return TRUE;
    }

    case CLIP_MSG_FILE_DATA:
        CreateDirectoryW(g_clip_temp_dir, NULL);
        return clip_reader_recv_file_data(d, s, hdr);

    default:
        idd_log(d, L"CLIP-R: Unknown msg_type %u.", hdr->msg_type);
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

/* Reader recv thread — handles guest→host clipboard via :0006 with auto-reconnect.
   Unlike :0005, the reader process runs as the logged-in user and may not be
   available at window creation time, so this thread handles initial connection
   as well as reconnection. */
static DWORD WINAPI clip_reader_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    SOCKET s;
    ClipHeader hdr;

    idd_log(d, L"CLIP-R: Recv thread started.");

    while (!d->stop) {
        /* Connect (or reconnect) to :0006 */
        while (!d->stop) {
            int wait;
            SOCKET rs;

            /* If we already have a socket (initial connect succeeded), use it */
            if (d->clip_reader_socket != INVALID_SOCKET) break;

            rs = connect_to_hv_service(&d->runtime_id, &CLIPBOARD_READER_SERVICE_GUID, 2000);
            if (rs != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(rs, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(rs, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    d->clip_reader_socket = rs;
                    idd_log(d, L"CLIP-R: Connected (GUID :0006).");
                    break;
                }
                closesocket(rs);
            }

            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
        }

        if (d->stop) break;

        /* Message loop */
        s = d->clip_reader_socket;
        while (!d->stop) {
            if (!recv_exact(s, &hdr, sizeof(hdr)))
                break;
            if (hdr.magic != CLIP_MAGIC) {
                idd_log(d, L"CLIP-R: Bad magic 0x%08X.", hdr.magic);
                break;
            }
            if (!clip_reader_handle_message(d, s, &hdr))
                break;
        }

        /* Cleanup after disconnect */
        clip_reader_free_pending(d);
        clip_remove_dir_recursive(g_clip_temp_dir);
        closesocket(d->clip_reader_socket);
        d->clip_reader_socket = INVALID_SOCKET;

        if (d->stop) break;
        idd_log(d, L"CLIP-R: Disconnected, will retry...");
    }

    d->clip_reader_recv_thread = NULL;
    idd_log(d, L"CLIP-R: Recv thread exiting.");
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

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    HRESULT hr;

    /* Create device and swap chain */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = DEFAULT_WIDTH;
    scd.BufferDesc.Height                  = DEFAULT_HEIGHT;
    scd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = d->render_hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION,
        &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);

    if (FAILED(hr)) {
        ui_log(L"IDD: D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        return FALSE;
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

    /* Create frame texture (dynamic, CPU-writable) */
    ZeroMemory(&td, sizeof(td));
    td.Width              = DEFAULT_WIDTH;
    td.Height             = DEFAULT_HEIGHT;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DYNAMIC;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

    hr = d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &d->frame_tex);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateTexture2D failed (0x%08X)", hr);
        return FALSE;
    }

    /* Shader resource view for the frame texture */
    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    hr = d->device->lpVtbl->CreateShaderResourceView(d->device,
            (ID3D11Resource *)d->frame_tex, &srv_desc, &d->frame_srv);
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
    if (rc.right == 0 || rc.bottom == 0) return;

    hr = d->swap_chain->lpVtbl->ResizeBuffers(d->swap_chain, 0,
            (UINT)rc.right, (UINT)rc.bottom,
            DXGI_FORMAT_UNKNOWN, 0);
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
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp;
    RECT rc;
    HRESULT hr;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!d->device || !d->ctx || !d->swap_chain || !d->rtv)
        return;

    /* Upload frame data to GPU texture if dirty */
    if (d->frame_dirty) {
        EnterCriticalSection(&d->frame_cs);
        hr = d->ctx->lpVtbl->Map(d->ctx,
                (ID3D11Resource *)d->frame_tex, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            UINT row;
            UINT copy_stride = d->frame_width * 4;
            if (copy_stride > mapped.RowPitch)
                copy_stride = mapped.RowPitch;
            if (copy_stride > d->frame_stride)
                copy_stride = d->frame_stride;

            for (row = 0; row < d->frame_height && row < DEFAULT_HEIGHT; row++) {
                memcpy((BYTE *)mapped.pData + row * mapped.RowPitch,
                       d->frame_buf + row * d->frame_stride,
                       copy_stride);
            }
            d->ctx->lpVtbl->Unmap(d->ctx,
                    (ID3D11Resource *)d->frame_tex, 0);
        }
        d->frame_dirty = FALSE;
        LeaveCriticalSection(&d->frame_cs);
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

    if (d->render_count % 60 == 0 && d->hwnd) {
        wchar_t title[256];
        swprintf_s(title, 256, L"%s%s Display %ux%u recv=%u",
                   d->audio_muted ? L"\U0001F507 " : L"",
                   d->vm_name, d->frame_width, d->frame_height, d->recv_count);
        SetWindowTextW(d->hwnd, title);
    }
    d->render_count++;

    d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
    d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &vp);
    d->ctx->lpVtbl->ClearRenderTargetView(d->ctx, d->rtv, clear_color);

    d->ctx->lpVtbl->IASetPrimitiveTopology(d->ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->lpVtbl->IASetInputLayout(d->ctx, NULL);

    d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
    d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps, NULL, 0);
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &d->frame_srv);
    d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);

    /* Draw fullscreen triangle (3 vertices, no vertex buffer) */
    d->ctx->lpVtbl->Draw(d->ctx, 3, 0);

    d->swap_chain->lpVtbl->Present(d->swap_chain, 0, 0);
}

static void d3d_cleanup(VmDisplayIdd *d)
{
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

/* ==================================================================
 * Guest cursor — create HCURSOR from received bitmap
 * ================================================================== */

/* Cursor types from IddCx IDDCX_CURSOR_SHAPE_TYPE */
#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

static HCURSOR create_cursor_from_bitmap(UINT width, UINT height,
                                          UINT xhot, UINT yhot,
                                          UINT cursor_type, UINT pitch,
                                          const BYTE *shape_data)
{
    HCURSOR result = NULL;
    BITMAPINFO bmi;
    HBITMAP hColor = NULL, hMask = NULL;
    ICONINFO ii;
    HDC hdc;

    if (width == 0 || height == 0 || width > 256 || height > 256)
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
        UINT mask_pitch = ((mask_row_bytes + 3) & ~3u);
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
           Height is the real cursor height. */
        void *color_bits = NULL;
        UINT row, dst_pitch;

        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)width;
        bmi.bmiHeader.biHeight      = -(LONG)height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &color_bits, NULL, 0);
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

        /* All-zero mask = fully controlled by color bitmap alpha channel.
           CreateBitmap with NULL bits is uninitialized — allocate zeroed. */
        {
            UINT mbytes = (((width + 7) / 8 + 3) & ~3u) * height;
            BYTE *zbuf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, mbytes);
            hMask = CreateBitmap((int)width, (int)height, 1, 1, zbuf);
            if (zbuf) HeapFree(GetProcessHeap(), 0, zbuf);
        }

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

static DWORD WINAPI idd_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Allocate receive buffer for frame pixel data */
    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, MAX_FRAME_DATA_SIZE);
    if (!recv_buf) {
        ui_log(L"IDD recv: failed to allocate receive buffer");
        return 1;
    }

    /* Tell the agent to respawn input helper in console session */
    if (!d->stop && d->vm && d->vm->agent_online) {
        idd_log(d, L"Sending idd_connect to agent...");
        vm_agent_send(d->vm, "idd_connect", NULL, 0);
    }

    /* Input socket lives independently of the frame channel — survives
       frame disconnections so the user can still send input to wake
       the VM screen if the display path goes inactive. */
    {
        SOCKET input_s = INVALID_SOCKET;

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;

        /* Ensure input channel is connected (independent of frame channel) */
        if (input_s == INVALID_SOCKET) {
            input_s = connect_to_hv_service(&d->runtime_id, &INPUT_SERVICE_GUID, 1000);
            if (input_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(input_s, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == INPUT_READY_MAGIC) {
                    DWORD zero_timeout = 0;
                    u_long nb = 1;
                    setsockopt(input_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                    ioctlsocket(input_s, FIONBIO, &nb);
                    d->input_socket = input_s;
                    g_input_send_count = 0;
                    idd_log(d, L"Input connected + ready (GUID :0003).");
                } else {
                    idd_log(d, L"Input handshake failed - closing.");
                    closesocket(input_s);
                    input_s = INVALID_SOCKET;
                }
            }
        }

        /* Ensure clipboard writer channel is connected (:0005, host→guest) */
        if (d->clip_socket == INVALID_SOCKET && !d->clip_recv_thread) {
            SOCKET clip_s = connect_to_hv_service(&d->runtime_id, &CLIPBOARD_SERVICE_GUID, 1000);
            if (clip_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(clip_s, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(clip_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    d->clip_socket = clip_s;
                    d->clip_recv_thread = CreateThread(NULL, 0, clip_recv_thread_proc, d, 0, NULL);
                    idd_log(d, L"Clipboard writer connected + ready (GUID :0005).");
                } else {
                    idd_log(d, L"Clipboard writer handshake failed - closing.");
                    closesocket(clip_s);
                }
            }
        }

        /* Ensure clipboard reader recv thread is running (:0006, guest→host).
           The thread handles connecting on its own — the reader process runs as
           the logged-in user and may not be available yet at window creation. */
        if (!d->clip_reader_recv_thread) {
            d->clip_reader_recv_thread = CreateThread(NULL, 0, clip_reader_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"CLIP-R: Started recv thread (will connect when reader is available).");
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        s = connect_to_hv_service(&d->runtime_id, &FRAME_SERVICE_GUID, 3000);
        if (s == INVALID_SOCKET) {
            int wait;
            idd_log(d, L"Connection failed, retrying in 3s.");
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        idd_log(d, L"Frame channel connected.");

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;

            /* Peek at magic to determine message type */
            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;

                d->cursor_visible = chdr.visible;

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

                    if (!recv_exact(s, cursor_buf, (int)chdr.shape_data_size)) {
                        HeapFree(GetProcessHeap(), 0, cursor_buf);
                        break;
                    }

                    /* Create new cursor from bitmap */
                    {
                        HCURSOR new_cursor = create_cursor_from_bitmap(
                            chdr.width, chdr.height, chdr.xhot, chdr.yhot,
                            chdr.cursor_type, chdr.pitch, cursor_buf);
                        if (new_cursor) {
                            HCURSOR old = d->guest_cursor;
                            d->guest_cursor = new_cursor;
                            d->cursor_shape_id = chdr.shape_id;
                            if (old) DestroyCursor(old);
                            /* Force cursor update if mouse is in window */
                            if (d->render_hwnd)
                                PostMessageW(d->render_hwnd, WM_SETCURSOR,
                                             (WPARAM)d->render_hwnd,
                                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                        }
                    }

                    HeapFree(GetProcessHeap(), 0, cursor_buf);
                }
                continue;  /* back to message loop */
            }

            if (magic != FRAME_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }

            /* Read rest of frame header (already read magic) */
            hdr.magic = magic;
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
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
                if (!recv_exact(s, dirty_rects, (int)(rect_count * sizeof(RECT))))
                    break;
            }

            /* Read data_size */
            if (!recv_exact(s, &data_size, 4))
                break;

            if (data_size > MAX_FRAME_DATA_SIZE) {
                idd_log(d, L"Frame data too large (%u bytes), reconnecting.", data_size);
                break;
            }

            /* Read pixel data */
            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            /* Reallocate frame_buf if resolution changed */
            if (hdr.width != d->frame_width || hdr.height != d->frame_height) {
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
                    idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                            hdr.width, hdr.height, hdr.stride);
                    idd_log(d, L"Resolution changed to %ux%u.", hdr.width, hdr.height);
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
            d->recv_count++;
            LeaveCriticalSection(&d->frame_cs);

            /* Signal the window thread to repaint */
            if (d->hwnd && IsWindow(d->hwnd))
                PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);

            /* Reconnect input socket if send_input flagged it dead */
            if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
                idd_log(d, L"Input socket closed, will reconnect...");
            }
            if (input_s == INVALID_SOCKET) {
                SOCKET new_s = connect_to_hv_service(&d->runtime_id, &INPUT_SERVICE_GUID, 1000);
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
                        g_input_send_count = 0;
                        idd_log(d, L"Input reconnected + ready (GUID :0003).");
                    } else {
                        closesocket(new_s);
                    }
                }
                /* If connect/handshake fails, will retry next frame */
            }
        }

        /* Frame channel lost — close it but keep input alive */
        closesocket(s);
        idd_log(d, L"Frame channel disconnected, reconnecting...");

        /* Check if input is still alive (send_input may have flagged it dead) */
        if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
            closesocket(input_s);
            input_s = INVALID_SOCKET;
            idd_log(d, L"Input socket flagged dead, will reconnect.");
        }

        /* Wait before reconnecting frame channel */
        {
            int wait;
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
        }
    }

    /* Final cleanup — close input socket on thread exit */
    d->input_socket = INVALID_SOCKET;
    if (input_s != INVALID_SOCKET) {
        closesocket(input_s);
    }
    idd_log(d, L"Input disconnected.");

    } /* end input_s scope */

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
        return 1;
    }

    /* Dark mode title bar to match AppSandbox main window */
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* Add "Mute audio" toggle to the system menu (right-click title bar) */
    {
        HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
        if (sysmenu) {
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_AUDIO_MUTE, L"Mute audio");
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

        d->log_hwnd = CreateWindowExW(
            0, L"AppSandboxIddLog", log_title,
            WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL,
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
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start the recv thread now that the window and D3D11 are ready */
    d->recv_thread = CreateThread(NULL, 0, idd_recv_thread_proc, d, 0, NULL);
    if (!d->recv_thread) {
        ui_log(L"IDD: Failed to create recv thread.");
        d3d_cleanup(d);
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start a present timer for steady rendering */
    SetTimer(d->hwnd, IDT_PRESENT, PRESENT_MS, NULL);

    /* Message pump */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

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

    switch (msg) {
    case WM_SYSCOMMAND:
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
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            RemoveClipboardFormatListener(hwnd);

            /* Stop recv threads */
            d->stop = TRUE;

            /* Wait for clipboard writer recv thread (:0005) */
            if (d->clip_recv_thread) {
                if (d->clip_socket != INVALID_SOCKET) {
                    closesocket(d->clip_socket);
                    d->clip_socket = INVALID_SOCKET;
                }
                WaitForSingleObject(d->clip_recv_thread, 2000);
                CloseHandle(d->clip_recv_thread);
                d->clip_recv_thread = NULL;
            }

            /* Wait for clipboard reader recv thread (:0006) */
            if (d->clip_reader_recv_thread) {
                if (d->clip_reader_socket != INVALID_SOCKET) {
                    closesocket(d->clip_reader_socket);
                    d->clip_reader_socket = INVALID_SOCKET;
                }
                WaitForSingleObject(d->clip_reader_recv_thread, 2000);
                CloseHandle(d->clip_reader_recv_thread);
                d->clip_reader_recv_thread = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                if (d->audio_socket != INVALID_SOCKET) {
                    closesocket(d->audio_socket);
                    d->audio_socket = INVALID_SOCKET;
                }
                WaitForSingleObject(d->audio_recv_thread, 2000);
                CloseHandle(d->audio_recv_thread);
                d->audio_recv_thread = NULL;
            }

            /* Wait briefly for recv thread to exit */
            if (d->recv_thread) {
                WaitForSingleObject(d->recv_thread, 2000);
                CloseHandle(d->recv_thread);
                d->recv_thread = NULL;
            }

            d->open = FALSE;

            /* Close the separate log window */
            if (d->log_hwnd && IsWindow(d->log_hwnd))
                DestroyWindow(d->log_hwnd);
            d->log_hwnd = NULL;
            d->log_list_hwnd = NULL;

            /* Clean up D3D11 */
            d3d_cleanup(d);

            /* Clean up guest cursor */
            if (d->guest_cursor) {
                DestroyCursor(d->guest_cursor);
                d->guest_cursor = NULL;
            }

            /* Clean up clipboard pending data */
            clip_free_pending(d);
            clip_reader_free_pending(d);

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             1, (LPARAM)d->vm);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PRESENT);
        if (d) d->hwnd = NULL;
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        RECT wr;
        /* Minimum: 320x180 client area */
        wr.left = 0; wr.top = 0; wr.right = 320; wr.bottom = 180;
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;
        /* Max: native frame size */
        if (d && d->frame_width > 0 && d->frame_height > 0) {
            wr.left = 0; wr.top = 0;
            wr.right = (LONG)d->frame_width; wr.bottom = (LONG)d->frame_height;
            AdjustWindowRectEx(&wr, style, FALSE, exstyle);
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
            d3d_resize_swap_chain(d);
        }
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (d) d3d_render_frame(d);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_PRESENT && d) {
            if (d->frame_dirty)
                d3d_render_frame(d);
        }
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) d3d_render_frame(d);
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (d && d->clip_suppress) {
            idd_log(d, L"CLIP: WM_CLIPBOARDUPDATE suppressed (echo).");
            return 0;
        }
        if (d) {
            idd_log(d, L"CLIP: WM_CLIPBOARDUPDATE — sending format list to guest.");
            clip_send_format_list(d);
        }
        return 0;

    case WM_CLIP_READER_APPLY:
        if (d) {
            idd_log(d, L"CLIP-R: Applying reader clipboard data.");
            clip_reader_apply_format_list(d);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
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

            {
                UINT vx, vy;
                /* lp coords are relative to render child */
                window_to_vm_coords(d->render_hwnd,
                                    (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                                    d->frame_width, d->frame_height, &vx, &vy);
                send_input(d, INPUT_MOUSE_MOVE, vx, vy, 0);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (d) {
            d->mouse_in = FALSE;
            d->tracking = FALSE;
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (d && d->guest_cursor)
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
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
        return 0;

    case WM_RBUTTONDOWN:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
        return 0;
    case WM_RBUTTONUP:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
        return 0;

    case WM_MBUTTONDOWN:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
        return 0;
    case WM_MBUTTONUP:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
        return 0;

    case WM_MOUSEWHEEL:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;

    /* ---- Keyboard input forwarding (only when mouse is in render area) ---- */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        UINT32 flags = 0;
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        if (lp & (1 << 24)) flags |= 1;  /* extended key */
        if (d && d->mouse_in) send_input(d, INPUT_KEY, (UINT32)wp, scan, flags);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        UINT32 flags = 2;  /* key up */
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        if (lp & (1 << 24)) flags |= 1;  /* extended key */
        if (d && d->mouse_in) send_input(d, INPUT_KEY, (UINT32)wp, scan, flags);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Public API
 * ================================================================== */

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    VmDisplayIdd *d;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm           = vm;
    wcscpy_s(d->vm_name, 256, vm->name);
    d->runtime_id   = vm->runtime_id;
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->open         = TRUE;
    d->stop         = FALSE;
    d->input_socket       = INVALID_SOCKET;
    d->clip_socket        = INVALID_SOCKET;
    d->clip_reader_socket = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;

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
    InitializeCriticalSection(&d->clip_cs);
    InitializeCriticalSection(&d->clip_reader_cs);
    d->clip_fetch_idx = 0;
    d->clip_reader_fetch_idx = 0;
    clip_init_temp_dir();

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

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(
                1, &display->window_thread, FALSE, 5000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
        CloseHandle(display->window_thread);
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        WaitForSingleObject(display->recv_thread, 3000);
        CloseHandle(display->recv_thread);
    }

    /* clip_recv_thread is cleaned up by WM_CLOSE handler, but guard */
    if (display->clip_recv_thread) {
        if (display->clip_socket != INVALID_SOCKET) {
            closesocket(display->clip_socket);
            display->clip_socket = INVALID_SOCKET;
        }
        WaitForSingleObject(display->clip_recv_thread, 3000);
        CloseHandle(display->clip_recv_thread);
    }

    /* clip_reader_recv_thread guard */
    if (display->clip_reader_recv_thread) {
        if (display->clip_reader_socket != INVALID_SOCKET) {
            closesocket(display->clip_reader_socket);
            display->clip_reader_socket = INVALID_SOCKET;
        }
        WaitForSingleObject(display->clip_reader_recv_thread, 3000);
        CloseHandle(display->clip_reader_recv_thread);
    }

    /* audio_recv_thread guard */
    if (display->audio_recv_thread) {
        if (display->audio_socket != INVALID_SOCKET) {
            closesocket(display->audio_socket);
            display->audio_socket = INVALID_SOCKET;
        }
        WaitForSingleObject(display->audio_recv_thread, 3000);
        CloseHandle(display->audio_recv_thread);
    }

    DeleteCriticalSection(&display->frame_cs);
    DeleteCriticalSection(&display->clip_cs);
    DeleteCriticalSection(&display->clip_reader_cs);
    clip_free_pending(display);
    clip_reader_free_pending(display);

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    HeapFree(GetProcessHeap(), 0, display);
}

BOOL vm_display_idd_is_open(VmDisplayIdd *display)
{
    if (!display) return FALSE;
    return display->open && display->hwnd && IsWindow(display->hwnd);
}
