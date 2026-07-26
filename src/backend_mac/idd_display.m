/*
 * idd_display.m -- live Windows-guest IDD desktop + input over ivshmem (ch2 + ch3).
 *
 * Production host-side IDD DISPLAY consumer for ivshmem Windows guests, the peer
 * of vz_display.m. ("IDD" = the guest's Indirect Display Driver / VDD; this is its
 * host-side window, not tied to QEMU — QEMU just happens to carry the transport.) It connects the VDD's ch2 DISPLAY stream and ch3 INPUT stream
 * through the existing AsbIvshmemTransport (-connectChannel:timeoutMs:), which
 * returns a blocking AF_UNIX fd bridged to the slot's rings by a background pump.
 * So this file never touches the rings directly: it blocking-reads the VDD wire
 * protocol off the ch2 fd into a framebuffer, and blocking-writes InputPacket
 * records to the ch3 fd. A reconnect (EOF) makes the VDD send a fresh full frame,
 * exactly like reopening the IDD window on a Windows host.
 *
 * The ring reads/writes go through the transport's fd bridge (the controller never touches
 * the rings directly), and all per-connection state lives in the controller so multiple VMs
 * each get their own window.
 */

#import "idd_display.h"
#import "asb_ivshmem_transport.h"
#import <AudioToolbox/AudioToolbox.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurface.h>

#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include "../../tools/transport/asb_transport.h"   /* ASB_CH_DISPLAY/INPUT/AUDIO/CLIPBOARD[_READER], AsbCursor, ASB_CURSOR_MAGIC */

/* ---- VDD wire protocol (mirror of tools/vdd/vdd.h + src/backend_win/vm_display_idd.c).
 * The VDD pushes the same stream it sends over HvSocket on a
 * Windows host: VDD_FRAME (header + dirty rects + pixels) and VDD_CURSOR (header + shape). ---- */
#define VDD_FRAME_MAGIC    0x52465341u        /* 'ASFR' */
#define VDD_CURSOR_MAGIC   0x52435341u        /* 'ASCR' */
#define VDD_MAX_DIRTY      64u
#pragma pack(push, 1)
typedef struct { uint32_t magic, width, height, stride; uint64_t frame_seq; uint32_t dirty_rect_count; } WireFrameHeader;
typedef struct { int32_t left, top, right, bottom; } WireRect;   /* Windows RECT */
typedef struct {
    uint32_t magic; int32_t x, y; uint32_t visible, shape_updated, shape_id,
             width, height, pitch, xhot, yhot, cursor_type, shape_data_size;
} WireCursorHeader;
#pragma pack(pop)

/* ---- ch3 input record (mirror of tools/agent/appsandbox-input.c's InputPacket). ---- */
#define INPUT_MAGIC        0x4E495341u        /* 'ASIN' */
#define INPUT_MOUSE_MOVE   0
#define INPUT_MOUSE_BUTTON 1
#define INPUT_MOUSE_WHEEL  2
#define INPUT_KEY          3
#define INPUT_BTN_LEFT     0
#define INPUT_BTN_RIGHT    1
#define INPUT_BTN_MIDDLE   2
#pragma pack(push, 1)
typedef struct { uint32_t magic, type, param1, param2, param3; } InputPacket;
#pragma pack(pop)

/* ---- ch4 AUDIO wire format (mirror of tools/agent/appsandbox-audio.c). The guest sends one
 * AudioHeader (format) then a stream of AudioFrameHeader+PCM. ---- */
#define AUDIO_HEADER_MAGIC 0x31415341u        /* 'ASA1' */
#define PCM_RING_SZ        (1u << 20)          /* 1 MiB PCM jitter buffer */
#pragma pack(push, 1)
typedef struct { uint32_t magic, sample_rate; uint16_t channels, bits_per_sample, format_tag, block_align; } AudioHeader;
typedef struct { uint32_t bytes; } AudioFrameHeader;
#pragma pack(pop)

/* ---- ch5/ch6 CLIPBOARD wire protocol (mirror of src/backend_win/vm_clipboard.c). Host is the
 * connector on both: ch5 = Mac->Win writer, ch6 = Win->Mac reader.
 * FORMAT_LIST -> DATA_REQ -> DATA_RESP; the guest sends CLIP_READY_MAGIC once it accepts. ---- */
#define CLIP_MAGIC          0x504C4341u       /* 'ACLP' */
#define CLIP_READY_MAGIC    0x59444C43u       /* 'CLDY' */
#define CLIP_MSG_FORMAT_LIST      1
#define CLIP_MSG_FORMAT_DATA_REQ  2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA        4
#define CLIP_MSG_SYNC_ENABLE      12
/* Windows clipboard format IDs we bridge (synthetic >=0xC000 for named/registered formats). */
#define WF_TEXT   13u                          /* CF_UNICODETEXT */
#define WF_DIB    8u                           /* CF_DIB */
#define WF_HDROP  15u                          /* CF_HDROP */
#define WF_HTML   0xC010u                      /* "HTML Format" */
#define WF_RTF    0xC011u                      /* "Rich Text Format" */
#pragma pack(push, 1)
typedef struct { uint32_t magic, msg_type, format, data_size; } ClipHeader;
typedef struct { uint32_t path_len; uint64_t file_size; uint8_t is_directory; } ClipFileInfo;
#pragma pack(pop)

/* pthread trampolines + the blocking fd reader/writer (defined after the controller; used by it). */
void *idd_display_thread(void *arg);
void *idd_input_thread(void *arg);
void *idd_audio_thread(void *arg);
void *idd_clip_writer_thread(void *arg);
void *idd_clip_reader_thread(void *arg);
int   idd_rd_full(int fd, void *buf, int len);
int   idd_wr_full(int fd, const void *buf, int len);
/* AudioQueue output callback: pull from the owning window's PCM jitter buffer (pad silence on underrun). */
static void idd_aq_cb(void *u, AudioQueueRef aq, AudioQueueBufferRef buf);
/* Clipboard format converters (Windows wire form <-> Mac pasteboard data). */
static NSData *idd_cfhtml_to_html(const uint8_t *d, uint32_t n);
static NSData *idd_html_to_cfhtml(NSData *frag);
static NSData *idd_dib_to_png(const uint8_t *d, uint32_t n);
static NSData *idd_image_to_dib(NSData *img);
/* Aspect-preserving fit (letterbox); shared by the Metal render (viewport/scissor), input mapping, and
   the cursor scale. Defined near IddDisplayView; forward-declared here for renderMetal above it. */
static CGRect idd_letterbox(double viewW, double viewH, double frameW, double frameH, double *outScale);

/* macOS virtual keycode -> Windows VK (US layout). Built once. */
static uint8_t g_vk[128];
static pthread_once_t g_vkOnce = PTHREAD_ONCE_INIT;
static void build_keymap(void)
{
    memset(g_vk, 0, sizeof(g_vk));
    /* letters */
    g_vk[0x00]='A'; g_vk[0x0B]='B'; g_vk[0x08]='C'; g_vk[0x02]='D'; g_vk[0x0E]='E';
    g_vk[0x03]='F'; g_vk[0x05]='G'; g_vk[0x04]='H'; g_vk[0x22]='I'; g_vk[0x26]='J';
    g_vk[0x28]='K'; g_vk[0x25]='L'; g_vk[0x2E]='M'; g_vk[0x2D]='N'; g_vk[0x1F]='O';
    g_vk[0x23]='P'; g_vk[0x0C]='Q'; g_vk[0x0F]='R'; g_vk[0x01]='S'; g_vk[0x11]='T';
    g_vk[0x20]='U'; g_vk[0x09]='V'; g_vk[0x0D]='W'; g_vk[0x07]='X'; g_vk[0x10]='Y';
    g_vk[0x06]='Z';
    /* number row */
    g_vk[0x12]='1'; g_vk[0x13]='2'; g_vk[0x14]='3'; g_vk[0x15]='4'; g_vk[0x17]='5';
    g_vk[0x16]='6'; g_vk[0x1A]='7'; g_vk[0x1C]='8'; g_vk[0x19]='9'; g_vk[0x1D]='0';
    /* whitespace / edit */
    g_vk[0x24]=0x0D; /* Return */     g_vk[0x35]=0x1B; /* Esc */
    g_vk[0x31]=0x20; /* Space */      g_vk[0x30]=0x09; /* Tab */
    g_vk[0x33]=0x08; /* Backspace */  g_vk[0x75]=0x2E; /* Delete(fwd) */
    /* arrows */
    g_vk[0x7B]=0x25; g_vk[0x7C]=0x27; g_vk[0x7D]=0x28; g_vk[0x7E]=0x26;
    /* nav cluster */
    g_vk[0x73]=0x24; /* Home */ g_vk[0x77]=0x23; /* End */
    g_vk[0x74]=0x21; /* PgUp */ g_vk[0x79]=0x22; /* PgDn */
    /* function keys */
    g_vk[0x7A]=0x70; g_vk[0x78]=0x71; g_vk[0x63]=0x72; g_vk[0x76]=0x73;
    g_vk[0x60]=0x74; g_vk[0x61]=0x75; g_vk[0x62]=0x76; g_vk[0x64]=0x77;
    g_vk[0x65]=0x78; g_vk[0x6D]=0x79; g_vk[0x67]=0x7A; g_vk[0x6F]=0x7B;
    /* punctuation (US layout) */
    g_vk[0x29]=0xBA; g_vk[0x18]=0xBB; g_vk[0x2B]=0xBC; g_vk[0x1B]=0xBD;
    g_vk[0x2F]=0xBE; g_vk[0x2C]=0xBF; g_vk[0x32]=0xC0; g_vk[0x21]=0xDB;
    g_vk[0x2A]=0xDC; g_vk[0x1E]=0xDD; g_vk[0x27]=0xDE;
}
/* extended-key keycodes (arrows + nav + fwd-delete) */
static int is_extended(unsigned short kc)
{
    switch (kc) {
    case 0x7B: case 0x7C: case 0x7D: case 0x7E:   /* arrows */
    case 0x73: case 0x77: case 0x74: case 0x79:   /* home/end/pgup/pgdn */
    case 0x75:                                    /* fwd delete */
        return 1;
    }
    return 0;
}

/* Resolve the GUI console user (the one who will paste) so root-created clipboard files can be chowned
   to them. The daemon runs as root (for vmnet), where NSTemporaryDirectory() is root's PRIVATE temp,
   unreadable by the logged-in user — see -clipRecvFiles. Prefer /dev/console's owner (the logged-in GUI
   user); fall back to SUDO_USER. Returns NO if no non-root target can be identified. */
static BOOL idd_console_user(uid_t *uid, gid_t *gid)
{
    struct stat st;
    if (stat("/dev/console", &st) == 0 && st.st_uid != 0) {
        struct passwd *pw = getpwuid(st.st_uid);
        *uid = st.st_uid; *gid = pw ? pw->pw_gid : st.st_gid;
        return YES;
    }
    const char *su = getenv("SUDO_USER");
    if (su && *su) {
        struct passwd *pw = getpwnam(su);
        if (pw && pw->pw_uid != 0) { *uid = pw->pw_uid; *gid = pw->pw_gid; return YES; }
    }
    return NO;
}

/* ---- Build an NSCursor from the VDD cursor record. Both QueryHardwareCursor3 types are 32bpp BGRA:
   ALPHA(2)=premultiplied; MASKED_COLOR(1)=BGR with the alpha channel as the AND mask (0xFF=transparent).
   Mirrors create_cursor_from_bitmap in vm_display_idd.c. A hidden guest
   cursor maps to a fully transparent cursor so the pointer disappears over the view. ---- */
static void cur_data_free(void *info, const void *data, size_t size) { (void)info; (void)size; free((void *)data); }

static NSCursor *buildGuestCursor(AsbCursor *cur, double scale)
{
    if (scale <= 0) scale = 1.0;
    if (!cur->visible) {
        NSImage *blank = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        return [[NSCursor alloc] initWithImage:blank hotSpot:NSZeroPoint];
    }
    uint32_t w = cur->width, h = cur->height, pitch = cur->pitch, type = cur->cursor_type;
    if (!w || !h || !cur->image_size || (type != 1 && type != 2) ||
        pitch < w * 4 || (uint64_t)pitch * h > cur->image_size) return nil;
    const uint8_t *src = (const uint8_t *)cur + sizeof(AsbCursor);
    size_t outsz = (size_t)w * h * 4;
    uint8_t *cpy = malloc(outsz);
    if (!cpy) return nil;
    for (uint32_t row = 0; row < h; row++) {
        const uint8_t *s = src + (size_t)row * pitch;
        uint8_t *d = cpy + (size_t)row * w * 4;
        if (type == 2) {
            memcpy(d, s, (size_t)w * 4);                          /* ALPHA: premultiplied BGRA */
        } else {
            for (uint32_t c = 0; c < w; c++) {                    /* MASKED_COLOR: AND mask in alpha */
                uint8_t B = s[c*4+0], G = s[c*4+1], R = s[c*4+2], A = s[c*4+3];
                if (A == 0) {                                     /* opaque colour */
                    d[c*4+0] = B; d[c*4+1] = G; d[c*4+2] = R; d[c*4+3] = 255;
                } else if (B || G || R) {                         /* XOR/invert pixel (e.g. I-beam strokes) */
                    d[c*4+0] = 0; d[c*4+1] = 0; d[c*4+2] = 0; d[c*4+3] = 255;  /* approximate as opaque black */
                } else {                                          /* transparent */
                    d[c*4+0] = 0; d[c*4+1] = 0; d[c*4+2] = 0; d[c*4+3] = 0;
                }
            }
        }
    }
    CGColorSpaceRef cs2 = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef cdp = CGDataProviderCreateWithData(NULL, cpy, outsz, cur_data_free);
    CGImageRef cg = CGImageCreate(w, h, 8, 32, w * 4, cs2,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, cdp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(cdp);
    CGColorSpaceRelease(cs2);
    if (!cg) return nil;
    /* Display at content scale (VM px * window/guest ratio). The NSImage is in points; the OS then
       applies the retina backing factor, so the cursor matches the scaled frame on a retina display. */
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w * scale, h * scale)];
    CGImageRelease(cg);
    return [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(cur->xhot * scale, cur->yhot * scale)];
}

/* ================================================================================
 * IddDisplayView -- frame-rendering + input NSView. The owning controller stashes the
 * reconstructed frame (render buffer) + cursor + geometry under its lock; this view's
 * render timer copies the working buffer into the render buffer when a new frame arrives
 * (so drawRect never races the ch2 reader) and applies the VDD HW cursor as its NSCursor.
 * Input (coalesced moves + discrete events) is written to the ch3 fd by the controller.
 * ================================================================================ */
@class IddDisplayWindow;

@interface IddDisplayView : NSView
@property (nonatomic, weak) IddDisplayWindow *owner;
@property (nonatomic, strong) NSTrackingArea *track;
- (void)updateGuestCursor;   /* mirror the guest HW cursor onto this view's NSCursor (render timer) */
@end

@interface IddDisplayWindow () <NSWindowDelegate>
@property (nonatomic, copy)   NSString *name;
@property (nonatomic, strong) AsbIvshmemTransport *transport;
@property (nonatomic, strong) IddDisplayView *view;
@property (nonatomic, strong) NSTimer *timer;
@property (nonatomic) BOOL borderlessFullscreen;
@property (nonatomic) NSWindowStyleMask windowedStyleMask;
@property (nonatomic) NSRect windowedFrame;

/* ch2/ch3/ch4/ch5/ch6 reader loops, run on pthreads via the C trampolines below. */
- (void)displayLoop;
- (void)inputLoop;
- (void)audioLoop;
- (void)clipWriterLoop;
- (void)clipReaderLoop;
/* ch4 audio: published fd (so teardown can shutdown() the blocking recv) + PCM jitter buffer. */
- (void)pcmPush:(const uint8_t *)p len:(uint32_t)n;
- (uint32_t)pcmPull:(uint8_t *)out len:(uint32_t)n;
- (void)pcmReset;
- (void)audioQueueStart:(const AudioHeader *)h;
/* ch5/ch6 clipboard: published fds for teardown + protocol helpers. */
- (void)setClipWriterFd:(int)fd;
- (void)setClipReaderFd:(int)fd;
- (BOOL)clipSendFormatList:(int)fd;
- (BOOL)clipServe:(int)fd format:(uint32_t)fmt;
- (BOOL)clipSendFileEntry:(int)fd full:(NSString *)full rel:(NSString *)rel;
- (BOOL)clipFetch:(int)fd format:(uint32_t)fmt pbType:(NSString *)pbType
            items:(NSMutableDictionary *)items urls:(NSMutableArray *)urls;
- (BOOL)clipRecvFiles:(int)fd urls:(NSMutableArray *)urls;
/* View callbacks (main thread): render-buffer + cursor access and input dispatch. */
- (void)renderTick;
- (void)renderMetal;   /* the view calls this on resize so the frame redraws synchronously while dragging */
- (uint32_t)frameWidth;
- (uint32_t)frameHeight;
- (NSCursor *)currentCursorForScale:(double)scale;
- (NSCursor *)appliedCursor;
- (void)sendInput:(uint32_t)type p1:(uint32_t)p1 p2:(uint32_t)p2 p3:(uint32_t)p3;
- (void)recordMoveX:(uint32_t)x y:(uint32_t)y;
- (void)flushMove;
@end

@implementation IddDisplayWindow {
    /* ch2 framebuffer, reconstructed from the VDD wire stream by the reader thread. The reader writes
       _fb (+ bumps _fbSeq under _fbLock); the render timer copies it once into a GPU surface when _fbSeq
       advances, so the GPU never races the reader. */
    uint8_t          *_fb;          /* working buffer (reader thread) */
    uint32_t          _fbW, _fbH, _fbStride;
    volatile uint32_t _fbSeq;       /* bumped on each applied frame */
    uint32_t          _renderSeq;   /* last _fbSeq uploaded to the GPU */
    pthread_mutex_t   _fbLock;

    /* Metal GPU render (zero-copy, mirrors the Windows D3D11 path). The reconstructed frame is copied
       ONCE into a pool of IOSurface-backed MTLTextures (the same Map+memcpy floor Windows has), then the
       GPU SAMPLES that surface in place — no CPU→GPU upload — with a LINEAR (bilinear) sampler into a
       letterboxed viewport+scissor, exactly like compute_letterbox + the D3D11 viewport. The CAMetalLayer
       is the view's backing layer; triple-buffered with an in-flight semaphore. */
    id<MTLDevice>              _mtlDev;
    id<MTLCommandQueue>        _mtlQ;
    id<MTLRenderPipelineState> _mtlPS;
    id<MTLSamplerState>        _mtlSamp;
    CAMetalLayer              *_metalLayer;   /* == the view's backing layer */
    dispatch_semaphore_t       _mtlSem;
    #define IDD_SURF_COUNT 3
    IOSurfaceRef               _surf[IDD_SURF_COUNT];
    id<MTLTexture>             _surfTex[IDD_SURF_COUNT];
    uint32_t                   _surfW, _surfH;
    int                        _frontIdx;     /* last surface written = what the GPU samples */
    BOOL                       _frontValid;
    CGSize                     _drawableSize;

    /* Guest cursor stashed by the ch2 reader (AsbCursor blob + shape bytes), applied on the
       main thread by the render timer (rebuilt on shape/visibility/scale change). */
    pthread_mutex_t   _curLock;
    uint8_t          *_curBlob;     /* sizeof(AsbCursor) + image bytes */
    volatile uint32_t _curBlobSeq;
    uint32_t          _curBlobApplied;
    double            _curScale;
    NSCursor         *_nsCursor;

    /* Coalesced mouse-move state (main thread only). A retina trackpad fires far more move
       events than the guest can inject, so we keep only the latest position and emit at most
       one move per render tick; discrete events flush the pending move first to keep ordering. */
    int               _hasMove;
    uint32_t          _moveX, _moveY;

    /* ch3 INPUT fd (the input WORKER thread connects + writes InputPacket). -1 until connected; the
       worker reconnects it. The main thread NEVER touches this fd — it only enqueues into _inq. */
    volatile int      _inputFd;
    pthread_mutex_t   _inputLock;

    /* Input send queue: the AppKit main thread (-sendInput:) enqueues InputPackets here and returns
       immediately; the input worker thread (inputLoop) drains it and does the actual ch3 send +
       reconnect. This is what makes the window structurally immune to a guest input-helper/agent/VDD
       restart — ch3 backpressure or a dead slot can only ever stall the worker, never the UI (or the
       HTTP API served on the main run loop). Bounded ring, drop-OLDEST when full (input is droppable;
       the latest mouse position wins). Guarded by _inqLock; the worker waits on _inqCond. */
    #define IDD_INQ_CAP 256
    InputPacket       _inq[IDD_INQ_CAP];
    uint32_t          _inqHead, _inqTail;
    pthread_mutex_t   _inqLock;
    pthread_cond_t    _inqCond;
    /* Display thread's current ch2 fd, published so teardown can shutdown() it to unblock the
       thread's blocking recv (idd_rd_full). Guarded by _inputLock (shared fd-publication lock). */
    volatile int      _displayFd;

    /* ch4 AUDIO. The reader thread connects ch4, reads the AudioHeader + PCM frames into the PCM
       jitter buffer; a CoreAudio AudioQueue callback pulls from it. _audioFd is published (like
       _displayFd) so teardown can shutdown() the blocking recv. */
    volatile int      _audioFd;
    AudioQueueRef     _aq;
    uint8_t          *_pcm;          /* PCM_RING_SZ jitter buffer */
    volatile uint32_t _pcmHead, _pcmTail;
    pthread_mutex_t   _pcmLock;

    /* ch5/ch6 CLIPBOARD published fds (guarded by _inputLock, the shared fd-publication lock). The
       NSPasteboard changeCount we set ourselves, so the writer doesn't echo a Windows->Mac paste
       straight back. */
    volatile int      _clipWriterFd;   /* ch5 Mac->Win */
    volatile int      _clipReaderFd;   /* ch6 Win->Mac */
    long              _clipSuppress;

    /* Reader threads + lifecycle. _stop is set on close; the threads connect, loop, and exit. */
    pthread_t         _displayThread;
    pthread_t         _inputThread;
    pthread_t         _audioThread;
    pthread_t         _clipWriterThread;
    pthread_t         _clipReaderThread;
    BOOL              _threadsStarted;
    volatile int      _stop;
}

- (instancetype)initWithName:(NSString *)name
                   transport:(AsbIvshmemTransport *)transport
{
    pthread_once(&g_vkOnce, build_keymap);

    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
    window.title = [NSString stringWithFormat:@"%@ — Display", name];
    [window setAcceptsMouseMovedEvents:YES];
    [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _name = [name copy];
    _transport = transport;
    _inputFd = -1;
    _displayFd = -1;
    _audioFd = -1;
    _clipWriterFd = -1;
    _clipReaderFd = -1;
    _clipSuppress = -1;
    _pcm = malloc(PCM_RING_SZ);
    pthread_mutex_init(&_fbLock, NULL);
    pthread_mutex_init(&_curLock, NULL);
    pthread_mutex_init(&_inputLock, NULL);
    pthread_mutex_init(&_pcmLock, NULL);
    pthread_mutex_init(&_inqLock, NULL);
    pthread_cond_init(&_inqCond, NULL);

    _view = [[IddDisplayView alloc] initWithFrame:frame];
    _view.owner = self;
    _view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = _view;
    window.delegate = self;
    [self setupMetal];
    return self;
}

/* Build the Metal device/pipeline/sampler and point the view's CAMetalLayer at them. The shaders are the
   direct MSL port of g_vs_hlsl/g_ps_hlsl in vm_display_idd.c: a fullscreen triangle (no vertex buffer)
   whose UVs sample the frame texture; the letterbox is done by the viewport+scissor at draw time, the
   bars by the clear color. Linear sampler + clamp == D3D11_FILTER_MIN_MAG_MIP_LINEAR + ADDRESS_CLAMP. */
- (void)setupMetal {
    _mtlDev = MTLCreateSystemDefaultDevice();
    if (!_mtlDev) return;   /* never nil on the Apple-Silicon hardware this app requires */
    _mtlQ = [_mtlDev newCommandQueue];

    NSString *src =
        @"#include <metal_stdlib>\n"
        @"using namespace metal;\n"
        @"struct VOut { float4 pos [[position]]; float2 uv; };\n"
        @"vertex VOut idd_vmain(uint vid [[vertex_id]]) {\n"
        @"    float2 uv = float2((vid << 1) & 2, vid & 2);\n"
        @"    VOut o; o.uv = uv;\n"
        @"    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        @"    return o;\n"
        @"}\n"
        @"fragment float4 idd_fmain(VOut in [[stage_in]],\n"
        @"        texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
        @"    return tex.sample(smp, in.uv);\n"
        @"}\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [_mtlDev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { NSLog(@"IDD Metal: library compile failed: %@", err); return; }
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction   = [lib newFunctionWithName:@"idd_vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"idd_fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _mtlPS = [_mtlDev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!_mtlPS) { NSLog(@"IDD Metal: pipeline failed: %@", err); return; }

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter    = MTLSamplerMinMagFilterLinear;   /* bilinear, like the D3D11 sampler */
    sd.magFilter    = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _mtlSamp = [_mtlDev newSamplerStateWithDescriptor:sd];
    _mtlSem  = dispatch_semaphore_create(IDD_SURF_COUNT);

    _metalLayer = (CAMetalLayer *)_view.layer;
    _metalLayer.device          = _mtlDev;
    _metalLayer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    _metalLayer.framebufferOnly = YES;
    _metalLayer.contentsScale   = self.window.backingScaleFactor ?: 1.0;
}

/* (Re)allocate the IOSurface pool + matching textures when the guest geometry changes. Each MTLTexture is
   created OVER its IOSurface, so the GPU samples the surface bytes directly (zero-copy upload). */
- (void)recreateSurfacesW:(uint32_t)w h:(uint32_t)h {
    for (int i = 0; i < IDD_SURF_COUNT; i++) {
        _surfTex[i] = nil;
        if (_surf[i]) { CFRelease(_surf[i]); _surf[i] = NULL; }
    }
    NSDictionary *props = @{ (id)kIOSurfaceWidth:           @(w),
                             (id)kIOSurfaceHeight:          @(h),
                             (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat:     @((uint32_t)(('B'<<24)|('G'<<16)|('R'<<8)|'A')) };
    for (int i = 0; i < IDD_SURF_COUNT; i++) {
        _surf[i] = IOSurfaceCreate((CFDictionaryRef)props);
        if (!_surf[i]) { _surfW = _surfH = 0; return; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:w height:h mipmapped:NO];
        td.usage       = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;   /* IOSurface is CPU+GPU shared memory (unified on Apple Silicon) */
        _surfTex[i] = [_mtlDev newTextureWithDescriptor:td iosurface:_surf[i] plane:0];
    }
    _surfW = w; _surfH = h; _frontValid = NO; _frontIdx = 0;
}

- (void)toggleBorderlessFullscreen:(id)sender {
    (void)sender;
    NSWindow *window = self.window;

    if (!self.borderlessFullscreen) {
        self.windowedStyleMask = window.styleMask;
        self.windowedFrame = window.frame;
        window.styleMask = NSWindowStyleMaskBorderless;
        [window setFrame:window.screen.frame display:YES];
        self.borderlessFullscreen = YES;
    } else {
        window.styleMask = self.windowedStyleMask;
        [window setFrame:self.windowedFrame display:YES];
        self.borderlessFullscreen = NO;
    }
}

- (void)showDisplay {
    [self showWindow:nil];
    /* Bring the window to the front. The headless daemon is an Accessory app, so its window would
       otherwise open behind the active app; activating brings it forward and gives it key focus.
       Harmless in the GUI (already the active app). Mirrors VzDisplayWindow.showDisplay. */
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];

    /* Start the ch2 (frame) + ch3 (input) reader threads + the 60 Hz render timer. On a REOPEN (the
       window was closed -> teardown set _stop=1, joined the threads, _threadsStarted=NO), clear the
       stop flag first or the recreated threads would see _stop and exit immediately -> a live window
       with dead display+input. Reconnecting re-arms ch2/ch3 and the guest re-accepts. */
    _stop = 0;
    self.userClosed = NO;
    if (!_threadsStarted) {
        _threadsStarted = YES;
        self.timer = [NSTimer timerWithTimeInterval:(1.0 / 60.0) repeats:YES block:^(NSTimer *t) {
            (void)t;
            [self renderTick];
        }];
        /* Common modes so renderTick keeps firing DURING a live window resize (event-tracking run-loop
           mode), not just the default mode — otherwise the frame freezes and CA stretches the last
           drawable to the new bounds while you drag the edge, snapping back only when the drag ends. */
        [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];
        pthread_create(&_displayThread, NULL, idd_display_thread, (__bridge void *)self);
        pthread_create(&_inputThread,   NULL, idd_input_thread,   (__bridge void *)self);
        /* ch4 audio + ch5/ch6 clipboard reader threads — same lifecycle as display/input: connect in
           a loop honoring _stop, publish their fds so teardown can shutdown() the blocking recv. */
        pthread_create(&_audioThread,      NULL, idd_audio_thread,       (__bridge void *)self);
        pthread_create(&_clipWriterThread, NULL, idd_clip_writer_thread, (__bridge void *)self);
        pthread_create(&_clipReaderThread, NULL, idd_clip_reader_thread, (__bridge void *)self);
    }
}

/* Render-timer body (main thread): flush a coalesced move, mirror the guest HW cursor onto the
   macOS cursor, and copy a freshly reconstructed frame into the render buffer + redraw. */
- (void)renderTick {
    [self flushMove];
    [self.view updateGuestCursor];
    [self renderMetal];
}

#pragma mark - GPU render (Metal, main thread)

/* Upload a new frame into the next pool surface (ONE copy — the Map+memcpy floor), or just re-encode on
   resize, then draw the letterboxed quad sampling the IOSurface in place. Mirrors d3d_render_frame. */
- (void)renderMetal {
    if (!_mtlPS || !_metalLayer) return;
    /* While the user is live-resizing the window, present synchronously inside the CA transaction (see the
       present branch below) so the new drawable and the layer's new bounds change atomically — no stale
       stretched/skewed frame. Outside resize we keep the cheap async present (no main-thread GPU wait). */
    BOOL liveResize = self.view.inLiveResize;
    if (_metalLayer.presentsWithTransaction != liveResize) _metalLayer.presentsWithTransaction = liveResize;
    CGFloat sc = _metalLayer.contentsScale ?: 1.0;
    CGSize want = CGSizeMake(self.view.bounds.size.width * sc, self.view.bounds.size.height * sc);
    if (want.width < 1 || want.height < 1) return;
    BOOL resized = !CGSizeEqualToSize(_drawableSize, want);
    if (resized) { _metalLayer.drawableSize = want; _drawableSize = want; }

    BOOL newFrame = (_fbSeq != _renderSeq) && _fb && _fbW && _fbH;
    if (!newFrame && !resized) return;             /* nothing changed -> no GPU work (idle = 0 draws) */

    int i;
    if (newFrame) {
        if (_surfW != _fbW || _surfH != _fbH) [self recreateSurfacesW:_fbW h:_fbH];
        if (!_surf[0]) { _renderSeq = _fbSeq; return; }
        i = _frontValid ? (_frontIdx + 1) % IDD_SURF_COUNT : 0;
    } else {
        if (!_frontValid) return;                  /* resize before any frame -> nothing to show yet */
        i = _frontIdx;
    }

    /* Acquire a slot BEFORE touching surface[i], so the semaphore (count == surface count) guarantees its
       previous GPU read has finished — we never rewrite a surface the GPU is still sampling. Every draw
       (new frame OR resize redraw) takes exactly one slot, so the rotation/slot pairing stays balanced. */
    dispatch_semaphore_wait(_mtlSem, DISPATCH_TIME_FOREVER);
    if (newFrame) {
        IOSurfaceLock(_surf[i], 0, NULL);
        uint8_t *dst = (uint8_t *)IOSurfaceGetBaseAddress(_surf[i]);
        size_t dstStride = IOSurfaceGetBytesPerRow(_surf[i]);
        size_t dstRows = IOSurfaceGetHeight(_surf[i]);
        pthread_mutex_lock(&_fbLock);
        size_t copyStride = _fbStride < dstStride ? _fbStride : dstStride;
        /* Clamp rows to the surface height: the display thread may have grown _fbH after the
           surface was sized (above, from an unlocked read), so never write past the IOSurface. */
        for (uint32_t r = 0; r < _fbH && r < dstRows; r++)
            memcpy(dst + (size_t)r * dstStride, _fb + (size_t)r * _fbStride, copyStride);
        _renderSeq = _fbSeq;
        pthread_mutex_unlock(&_fbLock);
        IOSurfaceUnlock(_surf[i], 0, NULL);
        _frontIdx = i; _frontValid = YES;
    }

    id<CAMetalDrawable> drawable = [_metalLayer nextDrawable];
    if (!drawable) { dispatch_semaphore_signal(_mtlSem); return; }   /* transient -> skip this frame */

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = drawable.texture;
    rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.05, 0.05, 0.07, 1.0);   /* letterbox bars */
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [_mtlQ commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

    double dw = _drawableSize.width, dh = _drawableSize.height;
    CGRect lb = idd_letterbox(dw, dh, _surfW, _surfH, NULL);   /* same fit math as input + Windows */
    [enc setViewport:(MTLViewport){ lb.origin.x, lb.origin.y, lb.size.width, lb.size.height, 0.0, 1.0 }];
    NSUInteger sx = (NSUInteger)floor(lb.origin.x), sy = (NSUInteger)floor(lb.origin.y);
    NSUInteger sw = (NSUInteger)floor(lb.size.width), sh = (NSUInteger)floor(lb.size.height);
    if (sx + sw > (NSUInteger)dw) sw = (NSUInteger)dw - sx;
    if (sy + sh > (NSUInteger)dh) sh = (NSUInteger)dh - sy;
    if (sw && sh) [enc setScissorRect:(MTLScissorRect){ sx, sy, sw, sh }];   /* clip to the frame rect */

    [enc setRenderPipelineState:_mtlPS];
    [enc setFragmentTexture:_surfTex[i] atIndex:0];
    [enc setFragmentSamplerState:_mtlSamp atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];   /* fullscreen triangle */
    [enc endEncoding];
    dispatch_semaphore_t sem = _mtlSem;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cmd) { (void)cmd; dispatch_semaphore_signal(sem); }];
    if (liveResize) {
        /* presentsWithTransaction path: commit, wait until the GPU has SCHEDULED the work (cheap — not
           full completion), then present the drawable so it lands in the SAME CA transaction as the
           window's bounds change. This is what keeps the frame from stretching mid-drag. */
        [cb commit];
        [cb waitUntilScheduled];
        [drawable present];
    } else {
        [cb presentDrawable:drawable];
        [cb commit];
    }
}

- (uint32_t)frameWidth  { return _fbW; }
- (uint32_t)frameHeight { return _fbH; }

#pragma mark - Cursor access (main thread, render timer)

/* Apply the cursor stashed by the ch2 reader (built from the VDD wire cursor header). Rebuild
   when the shape/visibility changes or the window scale changes (retina-aware). Returns the
   NSCursor to install, or nil if nothing changed. */
- (NSCursor *)currentCursorForScale:(double)scale {
    pthread_mutex_lock(&_curLock);
    uint32_t seq = _curBlobSeq;
    if ((seq == _curBlobApplied && fabs(scale - _curScale) < 0.01) || !_curBlob) {
        pthread_mutex_unlock(&_curLock);
        return nil;
    }
    NSCursor *nc = buildGuestCursor((AsbCursor *)_curBlob, scale);
    pthread_mutex_unlock(&_curLock);
    if (nc) { _curBlobApplied = seq; _curScale = scale; _nsCursor = nc; }
    return nc;
}
- (NSCursor *)appliedCursor { return _nsCursor; }

#pragma mark - Input (main thread -> ch3 fd)

- (void)sendInput:(uint32_t)type p1:(uint32_t)p1 p2:(uint32_t)p2 p3:(uint32_t)p3 {
    InputPacket pkt = { INPUT_MAGIC, type, p1, p2, p3 };
    /* ENQUEUE ONLY — the AppKit main thread does ZERO ch3 socket I/O. The input worker thread
       (inputLoop) drains this ring and does the actual blocking send + owns reconnect. Any guest
       input-helper/agent/VDD restart (or desktop/session switch) can stall ch3, but that backpressure
       only ever stalls the worker thread — never the UI or the HTTP API served on the main run loop.
       (A non-blocking main-thread send would not suffice: the main thread stays coupled to ch3's live
       state and can pin in __sendto.) Bounded ring, drop-OLDEST when full (latest position wins). */
    pthread_mutex_lock(&_inqLock);
    uint32_t next = (_inqTail + 1) % IDD_INQ_CAP;
    if (next == _inqHead) _inqHead = (_inqHead + 1) % IDD_INQ_CAP;   /* full -> drop oldest */
    _inq[_inqTail] = pkt;
    _inqTail = next;
    pthread_cond_signal(&_inqCond);
    pthread_mutex_unlock(&_inqLock);
}

- (void)recordMoveX:(uint32_t)x y:(uint32_t)y { _moveX = x; _moveY = y; _hasMove = 1; }
- (void)flushMove {
    if (_hasMove) { _hasMove = 0; [self sendInput:INPUT_MOUSE_MOVE p1:_moveX p2:_moveY p3:0]; }
}

#pragma mark - Reader threads (frame + input)

/* Reconstruct the VDD wire stream from the ch2 fd into _fb. We connect via the transport (host is
   the connector; the VDD's asb_accept claims the slot and, on each fresh accept, sends a full frame
   first then dirty-rect frames + cursor updates). On EOF/error we close the fd and reconnect — a new
   accept => a fresh full frame, exactly like reopening the IDD window. Mirrors display_recv_thread. */
- (void)displayLoop {
    uint8_t *scratch = NULL;
    size_t   scratchSz = 0;
    while (!_stop) {
        int fd = [_transport connectChannel:ASB_CH_DISPLAY timeoutMs:2000];
        if (fd < 0) { if (_stop) break; usleep(200000); continue; }
        /* Publish the fd BEFORE the blocking read loop so teardown can shutdown() it to unblock us. */
        pthread_mutex_lock(&_inputLock);
        _displayFd = fd;
        pthread_mutex_unlock(&_inputLock);

        for (;;) {
            if (_stop) break;
            uint32_t magic;
            if (!idd_rd_full(fd, &magic, 4)) break;

            if (magic == VDD_CURSOR_MAGIC) {
                WireCursorHeader ch;
                ch.magic = magic;
                if (!idd_rd_full(fd, (uint8_t *)&ch + 4, sizeof(ch) - 4)) break;
                uint8_t *shape = NULL;
                if (ch.shape_updated && ch.shape_data_size > 0) {
                    if (ch.shape_data_size > 4u*1024*1024) break;   /* sanity */
                    shape = malloc(ch.shape_data_size);
                    if (!shape) break;
                    if (!idd_rd_full(fd, shape, (int)ch.shape_data_size)) { free(shape); break; }
                }
                /* Build an AsbCursor blob (header + image) so the render timer reuses buildGuestCursor
                   unchanged. shape_updated=0 => keep last image, update visibility only. */
                pthread_mutex_lock(&_curLock);
                if (shape) {
                    uint8_t *blob = malloc(sizeof(AsbCursor) + ch.shape_data_size);
                    if (blob) {
                        AsbCursor *c = (AsbCursor *)blob;
                        memset(c, 0, sizeof(*c));
                        c->magic = ASB_CURSOR_MAGIC;
                        c->visible = ch.visible; c->width = ch.width; c->height = ch.height;
                        c->pitch = ch.pitch; c->xhot = ch.xhot; c->yhot = ch.yhot;
                        c->cursor_type = ch.cursor_type; c->image_size = ch.shape_data_size;
                        memcpy(blob + sizeof(AsbCursor), shape, ch.shape_data_size);
                        free(_curBlob); _curBlob = blob; _curBlobSeq++;
                    }
                } else if (_curBlob) {
                    AsbCursor *c = (AsbCursor *)_curBlob;
                    if (c->visible != ch.visible) { c->visible = ch.visible; _curBlobSeq++; }
                }
                pthread_mutex_unlock(&_curLock);
                free(shape);
                continue;
            }

            if (magic != VDD_FRAME_MAGIC) break;   /* desync -> reconnect */

            WireFrameHeader fh;
            fh.magic = magic;
            if (!idd_rd_full(fd, (uint8_t *)&fh + 4, sizeof(fh) - 4)) break;
            if (fh.width == 0 || fh.height == 0 || fh.stride < fh.width * 4 ||
                fh.width > 8192 || fh.height > 8192) break;

            /* (Re)allocate the working framebuffer if geometry changed. */
            if (_fbW != fh.width || _fbH != fh.height || _fbStride != fh.stride || !_fb) {
                uint8_t *nb = malloc((size_t)fh.stride * fh.height);
                if (!nb) break;
                pthread_mutex_lock(&_fbLock);
                free(_fb); _fb = nb;
                _fbW = fh.width; _fbH = fh.height; _fbStride = fh.stride;
                memset(_fb, 0, (size_t)_fbStride * _fbH);
                pthread_mutex_unlock(&_fbLock);
            }

            if (fh.dirty_rect_count == 0) {
                /* Full frame: data_size then height*stride bytes. Read into scratch FIRST (unlocked)
                   so a multi-MB blocking recv never holds _fbLock: renderTick (main thread) also takes
                   _fbLock, so holding it across the recv would stall the whole UI for the transfer.
                   _fbLock is taken only for the memcpy into _fb (as in the dirty-rect path below). */
                uint32_t data_size = 0;
                if (!idd_rd_full(fd, &data_size, 4)) break;
                uint32_t want = _fbStride * _fbH;
                if (data_size != want) break;
                if (data_size > scratchSz) { free(scratch); scratch = malloc(data_size); scratchSz = data_size; if (!scratch) break; }
                if (!idd_rd_full(fd, scratch, (int)data_size)) break;
                pthread_mutex_lock(&_fbLock);
                memcpy(_fb, scratch, data_size);
                _fbSeq++;
                pthread_mutex_unlock(&_fbLock);
            } else {
                /* Dirty rects: rects[], data_size, then per-rect packed rows. */
                if (fh.dirty_rect_count > VDD_MAX_DIRTY) break;
                WireRect rects[VDD_MAX_DIRTY];
                if (!idd_rd_full(fd, rects, (int)(fh.dirty_rect_count * sizeof(WireRect)))) break;
                uint32_t data_size = 0;
                if (!idd_rd_full(fd, &data_size, 4)) break;
                if (data_size > scratchSz) { free(scratch); scratch = malloc(data_size); scratchSz = data_size; if (!scratch) break; }
                if (!idd_rd_full(fd, scratch, (int)data_size)) break;
                pthread_mutex_lock(&_fbLock);
                size_t off = 0;
                for (uint32_t i = 0; i < fh.dirty_rect_count; i++) {
                    int left = rects[i].left, top = rects[i].top, right = rects[i].right, bot = rects[i].bottom;
                    if (left < 0) left = 0; if (top < 0) top = 0;
                    if (right > (int)_fbW) right = (int)_fbW;
                    if (bot > (int)_fbH) bot = (int)_fbH;
                    if (left >= right || top >= bot) continue;
                    uint32_t rw = (uint32_t)(right - left), rh = (uint32_t)(bot - top);
                    for (uint32_t row = 0; row < rh; row++) {
                        if (off + rw * 4 > data_size) { i = fh.dirty_rect_count; break; }
                        memcpy(_fb + (size_t)(top + row) * _fbStride + (size_t)left * 4,
                               scratch + off, rw * 4);
                        off += rw * 4;
                    }
                }
                _fbSeq++;
                pthread_mutex_unlock(&_fbLock);
            }
        }

        pthread_mutex_lock(&_inputLock);
        if (_displayFd == fd) _displayFd = -1;   /* unpublish before close so teardown won't shutdown a reused fd */
        pthread_mutex_unlock(&_inputLock);
        close(fd);   /* tears the pump down + releases the slot; reconnect = a fresh VDD full frame */
        if (!_stop) usleep(100000);
    }
    free(scratch);
}

/* Own the ch3 INPUT connection end-to-end on this worker thread: connect, publish the fd (so teardown
   can shutdown() it), then loop draining the main thread's _inq and BLOCKING-sending each InputPacket,
   plus detecting guest-side teardown. Blocking I/O is safe HERE (off the UI thread): ch3 backpressure
   or a dead slot stalls only this thread, and we recover by reconnecting. The main thread only ever
   touches _inq, so it can never wedge. Mirrors the proven Windows model (recv thread owns the input
   socket + its reconnect) — just with the send moved off the UI thread via the queue. */
- (void)inputLoop {
    while (!_stop) {
        int fd = [_transport connectChannel:ASB_CH_INPUT timeoutMs:2000];
        if (fd < 0) { if (_stop) break; usleep(200000); continue; }
        /* EPIPE not SIGPIPE: when the transport pump tears this slot down on guest-acceptor death it
           closes our socketpair peer, so a send here must fail with EPIPE (-> reconnect), never signal. */
        { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
        /* Genuinely non-blocking, mirroring Windows' ioctlsocket(FIONBIO) on the input socket
           (vm_display_idd.c:1560): macOS does not reliably honor MSG_DONTWAIT on AF_UNIX SOCK_STREAM, so
           without O_NONBLOCK the worker can still briefly block in __sendto on a full ring. With it, send
           returns EAGAIN instantly (-> drop) and only a real error/EOF triggers reconnect. */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        pthread_mutex_lock(&_inputLock);
        _inputFd = fd;
        pthread_mutex_unlock(&_inputLock);

        BOOL dead = NO;
        while (!_stop && !dead) {
            /* 1) Wait briefly for queued input, then drain it into a local batch. The cond wait wakes
               immediately when -sendInput: enqueues, or after 50 ms so we still periodically poll the
               fd for EOF (a guest input-helper respawn while input is idle). */
            InputPacket batch[IDD_INQ_CAP];
            int nbatch = 0;
            pthread_mutex_lock(&_inqLock);
            if (_inqHead == _inqTail && !_stop) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50 * 1000 * 1000;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&_inqCond, &_inqLock, &ts);
            }
            while (_inqHead != _inqTail && nbatch < IDD_INQ_CAP) {
                batch[nbatch++] = _inq[_inqHead];
                _inqHead = (_inqHead + 1) % IDD_INQ_CAP;
            }
            pthread_mutex_unlock(&_inqLock);

            /* 2) Send the batch NON-BLOCKING — mirrors the proven Windows send_input
               (vm_display_idd.c:504). EAGAIN/EWOULDBLOCK = ch3 send buffer momentarily full -> DROP the
               packet, keep the connection (input is droppable; never block this worker waiting on the
               guest). Any OTHER error, or a partial write (which would misalign the guest's fixed 20-byte
               reads), means the channel is dead -> reconnect + resync on a fresh accept. The dead signal
               arrives because the transport pump tears the slot down + closes our fd when the guest
               acceptor stops draining (force-killed helper) — the ivshmem analog of an hvsocket RST — so
               this send then fails (EPIPE) exactly like Windows. A guest that is merely slow keeps
               draining, so EAGAIN-drop just sheds a few stale input events without dropping the link. */
            for (int i = 0; i < nbatch && !_stop; i++) {
                ssize_t wn = send(fd, &batch[i], sizeof(InputPacket), MSG_DONTWAIT);
                if (wn == (ssize_t)sizeof(InputPacket)) continue;            /* delivered */
                if (wn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;  /* full -> drop */
                dead = YES; break;                                          /* error/partial -> reconnect */
            }
            if (dead || _stop) break;

            /* 3) Detect a guest-side teardown even while idle: ch3 is host->guest, but the guest's
               socketpair end is closed by the pump on disconnect/respawn (asb_ivshmem_pump_main done:),
               and the input helper sends a one-time READY magic. Poll readable (0 timeout, non-blocking);
               recv==0 is EOF -> reconnect; recv>0 is the incidental READY byte(s) -> discard and keep
               serving. So input survives a guest restart even with nothing being typed. */
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
            int s = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (s < 0) break;                          /* select error -> reconnect */
            if (s > 0 && FD_ISSET(fd, &rfds)) {
                char drain[64];
                ssize_t rd = recv(fd, drain, sizeof(drain), 0);
                if (rd <= 0) break;   /* 0 = guest closed ch3 (respawn/disconnect) -> reconnect */
            }
        }

        pthread_mutex_lock(&_inputLock);
        if (_inputFd == fd) _inputFd = -1;
        pthread_mutex_unlock(&_inputLock);
        close(fd);
        if (!_stop) usleep(100000);
    }
}

#pragma mark - Audio (ch4 -> CoreAudio AudioQueue)

/* PCM jitter buffer (byte ring). The reader thread pushes; the AudioQueue callback pulls.
   Full => drop oldest so latency stays bounded. */
- (void)pcmPush:(const uint8_t *)p len:(uint32_t)n {
    pthread_mutex_lock(&_pcmLock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (_pcmTail + 1) % PCM_RING_SZ;
        if (next == _pcmHead) _pcmHead = (_pcmHead + 1) % PCM_RING_SZ;   /* full -> drop oldest */
        _pcm[_pcmTail] = p[i];
        _pcmTail = next;
    }
    pthread_mutex_unlock(&_pcmLock);
}
- (uint32_t)pcmPull:(uint8_t *)out len:(uint32_t)n {
    pthread_mutex_lock(&_pcmLock);
    uint32_t got = 0;
    while (got < n && _pcmHead != _pcmTail) { out[got++] = _pcm[_pcmHead]; _pcmHead = (_pcmHead + 1) % PCM_RING_SZ; }
    pthread_mutex_unlock(&_pcmLock);
    return got;
}
- (void)pcmReset { pthread_mutex_lock(&_pcmLock); _pcmHead = _pcmTail = 0; pthread_mutex_unlock(&_pcmLock); }

/* Drain the guest's PCM stream off the ch4 fd into the jitter buffer and play it via an AudioQueue.
   Connect in a loop (like displayLoop): a fresh accept re-sends the AudioHeader. On EOF/error we
   stop the queue and reconnect. The fd is published so teardown can shutdown() the blocking recv. */
- (void)audioLoop {
    while (!_stop) {
        int fd = [_transport connectChannel:ASB_CH_AUDIO timeoutMs:2000];
        if (fd < 0) { if (_stop) break; usleep(200000); continue; }
        pthread_mutex_lock(&_inputLock);
        _audioFd = fd;
        pthread_mutex_unlock(&_inputLock);

        AudioHeader h;
        if (idd_rd_full(fd, &h, sizeof(h)) && h.magic == AUDIO_HEADER_MAGIC) {
            [self pcmReset];
            [self audioQueueStart:&h];
            for (;;) {
                if (_stop) break;
                AudioFrameHeader fh;
                if (!idd_rd_full(fd, &fh, sizeof(fh))) break;
                uint32_t remaining = fh.bytes;
                uint8_t tmp[16384];
                while (remaining > 0) {
                    uint32_t chunk = remaining > sizeof(tmp) ? (uint32_t)sizeof(tmp) : remaining;
                    if (!idd_rd_full(fd, tmp, (int)chunk)) { remaining = 0; goto closed; }
                    [self pcmPush:tmp len:chunk];
                    remaining -= chunk;
                }
            }
        }
    closed:
        if (_aq) { AudioQueueStop(_aq, true); AudioQueueDispose(_aq, true); _aq = NULL; }
        pthread_mutex_lock(&_inputLock);
        if (_audioFd == fd) _audioFd = -1;
        pthread_mutex_unlock(&_inputLock);
        close(fd);
        if (!_stop) usleep(100000);
    }
}

/* Build + start the AudioQueue for the guest's PCM format. */
- (void)audioQueueStart:(const AudioHeader *)h {
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = h->sample_rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = (h->format_tag == 3) ? (kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked)
                                                  : (kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
    asbd.mChannelsPerFrame = h->channels;
    asbd.mBitsPerChannel   = h->bits_per_sample;
    asbd.mBytesPerFrame    = h->block_align ? h->block_align : (h->channels * h->bits_per_sample / 8);
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
    if (AudioQueueNewOutput(&asbd, idd_aq_cb, (__bridge void *)self, NULL, NULL, 0, &_aq) != noErr) { _aq = NULL; return; }
    UInt32 bufBytes = (UInt32)(asbd.mSampleRate * 0.02) * asbd.mBytesPerFrame;   /* ~20 ms */
    if (bufBytes < 1024) bufBytes = 4096;
    for (int i = 0; i < 4; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(_aq, bufBytes, &b) != noErr) continue;
        memset(b->mAudioData, 0, bufBytes);
        b->mAudioDataByteSize = bufBytes;
        AudioQueueEnqueueBuffer(_aq, b, 0, NULL);
    }
    AudioQueueStart(_aq, NULL);
}

#pragma mark - Clipboard fd publication (guarded by _inputLock)

- (void)setClipWriterFd:(int)fd {
    pthread_mutex_lock(&_inputLock);
    _clipWriterFd = fd;
    pthread_mutex_unlock(&_inputLock);
}
- (void)setClipReaderFd:(int)fd {
    pthread_mutex_lock(&_inputLock);
    _clipReaderFd = fd;
    pthread_mutex_unlock(&_inputLock);
}

#pragma mark - Clipboard (ch5 Mac->Win writer, ch6 Win->Mac reader)

/* ===== ch5: Mac -> Windows (copy-on-Mac). Watch the local pasteboard changeCount; on change push a
   FORMAT_LIST, then serve DATA_REQ -> DATA_RESP. Mirrors the writer path in src/backend_win/vm_clipboard.c.
   We never echo a change we made ourselves (Win->Mac paste sets _clipSuppress). ===== */
- (void)clipWriterLoop {
    while (!_stop) {
        int fd = [_transport connectChannel:ASB_CH_CLIPBOARD timeoutMs:2000];
        if (fd < 0) { if (_stop) break; usleep(200000); continue; }
        [self setClipWriterFd:fd];

        uint32_t ready = 0;
        if (idd_rd_full(fd, &ready, 4) && ready == CLIP_READY_MAGIC) {
            ClipHeader se = { CLIP_MAGIC, CLIP_MSG_SYNC_ENABLE, 0, 1 }; uint8_t on = 1;
            idd_wr_full(fd, &se, sizeof(se)); idd_wr_full(fd, &on, 1);
            long last = [[NSPasteboard generalPasteboard] changeCount];
            while (!_stop) {
                long cc = [[NSPasteboard generalPasteboard] changeCount];
                if (cc != last) { last = cc; if (cc != _clipSuppress) { if (![self clipSendFormatList:fd]) break; } }
                /* Non-blocking peek for an inbound DATA_REQ: the pump fd has no MSG_PEEK length API,
                   so we poll with a short select and read a header when readable. */
                fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
                struct timeval tv = { 0, 30000 };
                int rs = select(fd + 1, &rf, NULL, NULL, &tv);
                if (rs < 0) break;
                if (rs > 0 && FD_ISSET(fd, &rf)) {
                    ClipHeader h;
                    if (!idd_rd_full(fd, &h, sizeof(h))) break;
                    if (h.magic == CLIP_MAGIC && h.msg_type == CLIP_MSG_FORMAT_DATA_REQ)
                        if (![self clipServe:fd format:h.format]) break;
                }
            }
        }
        [self setClipWriterFd:-1];
        close(fd);
        if (!_stop) usleep(100000);
    }
}

/* Build + send the FORMAT_LIST describing the Mac pasteboard's current contents. */
- (BOOL)clipSendFormatList:(int)fd {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    uint8_t buf[256]; uint32_t o = 4, cnt = 0;
    NSArray *types = pb.types;
    BOOL hasFiles = [pb readObjectsForClasses:@[[NSURL class]]
                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }].count > 0;
    #define ADD_FMT(fid, nm) do { const char *_n = (nm); uint32_t _nl = _n ? (uint32_t)strlen(_n) : 0; \
        if (o + 8 + _nl <= sizeof(buf)) { *(uint32_t *)(buf + o) = (fid); o += 4; \
            *(uint32_t *)(buf + o) = _nl; o += 4; if (_nl) { memcpy(buf + o, _n, _nl); o += _nl; } cnt++; } } while (0)
    if (hasFiles)                                      ADD_FMT(WF_HDROP, NULL);
    if ([types containsObject:NSPasteboardTypeString]) ADD_FMT(WF_TEXT, NULL);
    if ([types containsObject:NSPasteboardTypeRTF])    ADD_FMT(WF_RTF, "Rich Text Format");
    if ([types containsObject:NSPasteboardTypeHTML])   ADD_FMT(WF_HTML, "HTML Format");
    if ([types containsObject:NSPasteboardTypePNG] || [types containsObject:NSPasteboardTypeTIFF]) ADD_FMT(WF_DIB, NULL);
    #undef ADD_FMT
    if (cnt == 0) return YES;
    *(uint32_t *)buf = cnt;
    ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FORMAT_LIST, 0, o };
    if (!idd_wr_full(fd, &h, sizeof(h))) return NO;
    return idd_wr_full(fd, buf, (int)o);
}

/* Serve a single DATA_REQ for `fmt` from the Mac pasteboard, converting to the Windows wire form. */
- (BOOL)clipServe:(int)fd format:(uint32_t)fmt {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSData *out = nil;
    if (fmt == WF_TEXT) {
        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (s) { NSMutableData *m = [[s dataUsingEncoding:NSUTF16LittleEndianStringEncoding] mutableCopy];
                 uint16_t z = 0; [m appendBytes:&z length:2]; out = m; }
    } else if (fmt == WF_RTF)  out = [pb dataForType:NSPasteboardTypeRTF];
    else if (fmt == WF_HTML) { NSData *hd = [pb dataForType:NSPasteboardTypeHTML]; if (hd) out = idd_html_to_cfhtml(hd); }
    else if (fmt == WF_DIB)  { NSData *img = [pb dataForType:NSPasteboardTypePNG] ?: [pb dataForType:NSPasteboardTypeTIFF];
                               if (img) out = idd_image_to_dib(img); }
    else if (fmt == WF_HDROP) {
        NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]]
                                   options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
        for (NSURL *u in urls) if (![self clipSendFileEntry:fd full:u.path rel:u.path.lastPathComponent]) return NO;
        ClipHeader term = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_RESP, WF_HDROP, 0 };
        return idd_wr_full(fd, &term, sizeof(term));
    }
    ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_RESP, fmt, (uint32_t)out.length };
    if (!idd_wr_full(fd, &h, sizeof(h))) return NO;
    if (out.length) return idd_wr_full(fd, out.bytes, (int)out.length);
    return YES;
}

/* Stream one Mac file/dir entry (and recurse for directories) as CLIP_MSG_FILE_DATA records. */
- (BOOL)clipSendFileEntry:(int)fd full:(NSString *)full rel:(NSString *)rel {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:full isDirectory:&isDir]) return YES;
    NSData *relU16 = [[rel stringByReplacingOccurrencesOfString:@"/" withString:@"\\"]
                       dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
    ClipFileInfo fi = { (uint32_t)relU16.length, 0, isDir ? 1 : 0 };
    if (isDir) {
        ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FILE_DATA, 0, (uint32_t)(sizeof(fi) + relU16.length) };
        if (!idd_wr_full(fd, &h, sizeof(h)) || !idd_wr_full(fd, &fi, sizeof(fi)) ||
            !idd_wr_full(fd, relU16.bytes, (int)relU16.length)) return NO;
        for (NSString *child in [fm contentsOfDirectoryAtPath:full error:nil])
            if (![self clipSendFileEntry:fd full:[full stringByAppendingPathComponent:child]
                                     rel:[rel stringByAppendingPathComponent:child]]) return NO;
    } else {
        NSData *fdata = [NSData dataWithContentsOfFile:full];
        fi.file_size = fdata.length;
        ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FILE_DATA, 0, (uint32_t)(sizeof(fi) + relU16.length) };
        if (!idd_wr_full(fd, &h, sizeof(h)) || !idd_wr_full(fd, &fi, sizeof(fi)) ||
            !idd_wr_full(fd, relU16.bytes, (int)relU16.length)) return NO;
        if (fdata.length && !idd_wr_full(fd, fdata.bytes, (int)fdata.length)) return NO;
    }
    return YES;
}

/* ===== ch6: Windows -> Mac (paste-from-Windows). The guest pushes a FORMAT_LIST on each Windows
   copy; we DATA_REQ each format we want and apply it to the Mac pasteboard (on the main thread).
   Mirrors the reader path in src/backend_win/vm_clipboard.c (fetch-and-apply + file receive). ===== */
- (void)clipReaderLoop {
    while (!_stop) {
        int fd = [_transport connectChannel:ASB_CH_CLIPBOARD_READER timeoutMs:2000];
        if (fd < 0) { if (_stop) break; usleep(200000); continue; }
        [self setClipReaderFd:fd];

        uint32_t ready = 0;
        if (idd_rd_full(fd, &ready, 4) && ready == CLIP_READY_MAGIC) {
            for (;;) {
                if (_stop) break;
                ClipHeader h;
                if (!idd_rd_full(fd, &h, sizeof(h))) break;
                if (h.magic != CLIP_MAGIC) break;
                if (h.msg_type != CLIP_MSG_FORMAT_LIST) {                  /* drain stray payloads */
                    if (h.data_size) { uint8_t *s = malloc(h.data_size); idd_rd_full(fd, s, (int)h.data_size); free(s); }
                    continue;
                }
                uint8_t *buf = malloc(h.data_size ? h.data_size : 1);
                if (!buf || !idd_rd_full(fd, buf, (int)h.data_size)) { free(buf); break; }
                BOOL hT=NO,hH=NO,hR=NO,hI=NO,hF=NO;
                uint32_t cnt = h.data_size >= 4 ? *(uint32_t *)buf : 0, o = 4;
                for (uint32_t i = 0; i < cnt && o + 8 <= h.data_size; i++) {
                    uint32_t fmt = *(uint32_t *)(buf + o); o += 4;
                    uint32_t nl = *(uint32_t *)(buf + o); o += 4;
                    if (nl > h.data_size - o) break;            /* name length must stay within the payload */
                    NSString *name = nl ? [[NSString alloc] initWithBytes:buf + o length:nl encoding:NSASCIIStringEncoding] : nil;
                    o += nl;
                    if (fmt == WF_TEXT) hT = YES;
                    else if (fmt == WF_HDROP) hF = YES;
                    else if (fmt == WF_DIB) hI = YES;
                    else if ([name isEqualToString:@"HTML Format"]) hH = YES;
                    else if ([name isEqualToString:@"Rich Text Format"]) hR = YES;
                    else if ([name isEqualToString:@"PNG"]) hI = YES;
                }
                free(buf);
                NSMutableDictionary *items = [NSMutableDictionary dictionary];
                NSMutableArray *urls = [NSMutableArray array];
                BOOL ok = YES;
                if (hF) ok = ok && [self clipFetch:fd format:WF_HDROP pbType:nil items:items urls:urls];
                if (hT) ok = ok && [self clipFetch:fd format:WF_TEXT pbType:NSPasteboardTypeString items:items urls:urls];
                if (hR) ok = ok && [self clipFetch:fd format:WF_RTF  pbType:NSPasteboardTypeRTF    items:items urls:urls];
                if (hH) ok = ok && [self clipFetch:fd format:WF_HTML pbType:NSPasteboardTypeHTML   items:items urls:urls];
                if (hI) ok = ok && [self clipFetch:fd format:WF_DIB  pbType:NSPasteboardTypePNG    items:items urls:urls];
                if (!ok) break;
                if (items.count || urls.count) {
                    NSDictionary *itemsCopy = [items copy];
                    NSArray *urlsCopy = [urls copy];
                    /* async, NOT sync: -teardown runs on the main thread and joins this thread, so a
                       sync hop to main would deadlock if the window closes mid-paste. The block keeps
                       self alive and sets _clipSuppress after the write, which the ch5 writer's
                       changeCount check tolerates (worst case one redundant echo). */
                    dispatch_async(dispatch_get_main_queue(), ^{     /* NSPasteboard writes belong on the main thread */
                        NSPasteboard *pb = [NSPasteboard generalPasteboard];
                        [pb clearContents];
                        if (urlsCopy.count) [pb writeObjects:urlsCopy];
                        for (NSString *t in itemsCopy) [pb setData:itemsCopy[t] forType:t];
                        self->_clipSuppress = [pb changeCount];      /* don't echo this back over ch5 */
                    });
                }
            }
        }
        [self setClipReaderFd:-1];
        close(fd);
        if (!_stop) usleep(100000);
    }
}

/* DATA_REQ one Windows format off ch6 and stash the converted bytes in `items` (or files in `urls`). */
- (BOOL)clipFetch:(int)fd format:(uint32_t)fmt pbType:(NSString *)pbType
            items:(NSMutableDictionary *)items urls:(NSMutableArray *)urls {
    ClipHeader req = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_REQ, fmt, 0 };
    if (!idd_wr_full(fd, &req, sizeof(req))) return NO;
    if (fmt == WF_HDROP) return [self clipRecvFiles:fd urls:urls];
    ClipHeader h;
    if (!idd_rd_full(fd, &h, sizeof(h)) || h.msg_type != CLIP_MSG_FORMAT_DATA_RESP) return NO;
    uint8_t *data = h.data_size ? malloc(h.data_size) : NULL;
    if (h.data_size && !idd_rd_full(fd, data, (int)h.data_size)) { free(data); return NO; }
    NSData *raw = data ? [NSData dataWithBytes:data length:h.data_size] : nil;
    free(data);
    if (!raw.length) return YES;
    if (fmt == WF_TEXT) {
        NSString *s = [[NSString alloc] initWithData:raw encoding:NSUTF16LittleEndianStringEncoding];
        if (!s) return YES;
        NSRange z = [s rangeOfString:@"\0"];
        if (z.location != NSNotFound) s = [s substringToIndex:z.location];
        NSData *u8 = [s dataUsingEncoding:NSUTF8StringEncoding];
        if (u8) items[pbType] = u8;
        return YES;
    }
    NSData *conv = nil;
    if (fmt == WF_HTML)     conv = idd_cfhtml_to_html(raw.bytes, (uint32_t)raw.length);
    else if (fmt == WF_DIB) conv = idd_dib_to_png(raw.bytes, (uint32_t)raw.length);
    else                    conv = raw;   /* RTF passthrough */
    if (conv.length) items[pbType] = conv;
    return YES;
}

/* Receive a CF_HDROP file set off ch6 into a temp dir and append the top-level NSURLs to `urls`. */
- (BOOL)clipRecvFiles:(int)fd urls:(NSMutableArray *)urls {
    NSFileManager *fm = [NSFileManager defaultManager];
    /* Stage the received files where the PASTING user can read them. When the daemon runs as root (for
       vmnet networking), NSTemporaryDirectory() is root's PRIVATE /var/folders/.../T (mode 700): the file
       transfers fine but the file URL we put on the pasteboard is then unreadable by the logged-in user,
       so paste yields nothing (text works only because it's inline pasteboard bytes, not a file ref). So
       when root, stage under world-traversable /tmp and hand the tree to the console user; otherwise keep
       the per-user temp (already user-owned). */
    uid_t cuid = 0; gid_t cgid = 0;
    BOOL chownToUser = (geteuid() == 0) && idd_console_user(&cuid, &cgid);
    NSString *base = chownToUser ? @"/tmp" : NSTemporaryDirectory();
    NSString *root = [base stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"asbclip-%u", arc4random()]];
    [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableArray *tops = [NSMutableArray array];
    for (;;) {
        ClipHeader h;
        if (!idd_rd_full(fd, &h, sizeof(h))) return NO;
        if (h.magic != CLIP_MAGIC) return NO;
        if (h.msg_type == CLIP_MSG_FORMAT_DATA_RESP) break;       /* terminator */
        if (h.msg_type != CLIP_MSG_FILE_DATA) {
            if (h.data_size) { uint8_t *s = malloc(h.data_size); idd_rd_full(fd, s, (int)h.data_size); free(s); }
            continue;
        }
        ClipFileInfo fi;
        if (!idd_rd_full(fd, &fi, sizeof(fi))) return NO;
        uint8_t *pb = malloc(fi.path_len ? fi.path_len : 1);
        if (fi.path_len && !idd_rd_full(fd, pb, (int)fi.path_len)) { free(pb); return NO; }
        NSString *rel = [[NSString alloc] initWithBytes:pb length:fi.path_len encoding:NSUTF16LittleEndianStringEncoding];
        free(pb);
        rel = [rel stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
        NSString *full = [root stringByAppendingPathComponent:rel];
        /* Path-traversal guard (the daemon runs as root): reject absolute paths / ".." components
           and require the standardized path to stay under `root`, so a malicious guest cannot
           escape the staging dir and write arbitrary host files. Unsafe entries are skipped, but
           their payload is still drained below so the stream stays aligned. */
        BOOL safe = (rel.length > 0) && ![rel hasPrefix:@"/"];
        if (safe) for (NSString *c in [rel pathComponents]) if ([c isEqualToString:@".."]) { safe = NO; break; }
        if (safe) {
            NSString *rootStd = [root stringByStandardizingPath];
            NSString *fullStd = [full stringByStandardizingPath];
            safe = [fullStd isEqualToString:rootStd] || [fullStd hasPrefix:[rootStd stringByAppendingString:@"/"]];
        }
        NSString *top = [[rel pathComponents] firstObject];
        if (safe && top && ![tops containsObject:top]) [tops addObject:top];
        if (fi.is_directory) {
            if (safe) [fm createDirectoryAtPath:full withIntermediateDirectories:YES attributes:nil error:nil];
        } else {
            FILE *f = NULL;
            if (safe) {
                [fm createDirectoryAtPath:[full stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
                f = fopen(full.fileSystemRepresentation, "wb");
            }
            uint64_t rem = fi.file_size; uint8_t buf[65536];
            while (rem > 0) {
                uint32_t chunk = rem > sizeof(buf) ? (uint32_t)sizeof(buf) : (uint32_t)rem;
                if (!idd_rd_full(fd, buf, (int)chunk)) { if (f) fclose(f); return NO; }
                if (f) fwrite(buf, 1, chunk, f);
                rem -= chunk;
            }
            if (f) fclose(f);
        }
    }
    /* Hand the received tree to the console user so the pasteboard file URL is readable on paste, and
       lock it to that user (0700) so other local users can't read clipboard contents from /tmp. */
    if (chownToUser) {
        chown(root.fileSystemRepresentation, cuid, cgid);
        chmod(root.fileSystemRepresentation, 0700);
        for (NSString *sub in [fm enumeratorAtPath:root])
            chown([root stringByAppendingPathComponent:sub].fileSystemRepresentation, cuid, cgid);
    }
    for (NSString *t in tops) [urls addObject:[NSURL fileURLWithPath:[root stringByAppendingPathComponent:t]]];
    return YES;
}

#pragma mark - NSWindowDelegate

- (void)windowWillClose:(NSNotification *)notification {
    self.userClosed = YES;   /* mark closed (X button or programmatic) before teardown */
    [self teardown];
}

#pragma mark - Teardown

- (void)teardown {
    if (_stop) return;
    _stop = 1;
    [self.timer invalidate];
    self.timer = nil;
    /* Wake the input worker if it's parked in pthread_cond_timedwait so it sees _stop at once
       (the shutdown() below also unblocks it if it's mid-send). */
    pthread_mutex_lock(&_inqLock);
    pthread_cond_broadcast(&_inqCond);
    pthread_mutex_unlock(&_inqLock);
    /* Drop both fds so the threads' blocking calls return: the input thread's published fd, and the
       display thread's published ch2 fd (it's blocked in idd_rd_full -> recv; shutdown() forces EOF).
       Without the display shutdown the join below hangs until the VDD next sends a frame. */
    pthread_mutex_lock(&_inputLock);
    int ifd = _inputFd; _inputFd = -1;
    int dfd = _displayFd; _displayFd = -1;
    int afd = _audioFd; _audioFd = -1;
    int cwfd = _clipWriterFd; _clipWriterFd = -1;
    int crfd = _clipReaderFd; _clipReaderFd = -1;
    pthread_mutex_unlock(&_inputLock);
    if (ifd >= 0) shutdown(ifd, SHUT_RDWR);
    if (dfd >= 0) shutdown(dfd, SHUT_RDWR);
    /* Audio + clipboard threads block in recv on their published fd; shutdown() forces EOF so the
       join below can't hang (same close-hang fix as display). */
    if (afd >= 0) shutdown(afd, SHUT_RDWR);
    if (cwfd >= 0) shutdown(cwfd, SHUT_RDWR);
    if (crfd >= 0) shutdown(crfd, SHUT_RDWR);
    if (_threadsStarted) {
        pthread_join(_displayThread, NULL);
        pthread_join(_inputThread, NULL);
        pthread_join(_audioThread, NULL);
        pthread_join(_clipWriterThread, NULL);
        pthread_join(_clipReaderThread, NULL);
        _threadsStarted = NO;
    }
    /* The audio thread stops its own AudioQueue on exit, but make sure nothing lingers. */
    if (_aq) { AudioQueueStop(_aq, true); AudioQueueDispose(_aq, true); _aq = NULL; }
}

- (void)dealloc {
    [self teardown];
    for (int i = 0; i < IDD_SURF_COUNT; i++) {
        _surfTex[i] = nil;
        if (_surf[i]) { CFRelease(_surf[i]); _surf[i] = NULL; }
    }
    free(_fb);
    free(_curBlob);
    free(_pcm);
    pthread_mutex_destroy(&_fbLock);
    pthread_mutex_destroy(&_curLock);
    pthread_mutex_destroy(&_inputLock);
    pthread_mutex_destroy(&_pcmLock);
    pthread_mutex_destroy(&_inqLock);
    pthread_cond_destroy(&_inqCond);
}

@end

/* ---- C trampolines for pthread_create (call back into the controller). ---- */
void *idd_display_thread(void *arg)
{
    @autoreleasepool { [(__bridge IddDisplayWindow *)arg displayLoop]; }
    return NULL;
}
void *idd_input_thread(void *arg)
{
    @autoreleasepool { [(__bridge IddDisplayWindow *)arg inputLoop]; }
    return NULL;
}
void *idd_audio_thread(void *arg)
{
    @autoreleasepool { [(__bridge IddDisplayWindow *)arg audioLoop]; }
    return NULL;
}
void *idd_clip_writer_thread(void *arg)
{
    @autoreleasepool { [(__bridge IddDisplayWindow *)arg clipWriterLoop]; }
    return NULL;
}
void *idd_clip_reader_thread(void *arg)
{
    @autoreleasepool { [(__bridge IddDisplayWindow *)arg clipReaderLoop]; }
    return NULL;
}

/* AudioQueue callback: fill the buffer from the owning window's PCM ring, padding silence on underrun. */
static void idd_aq_cb(void *u, AudioQueueRef aq, AudioQueueBufferRef buf)
{
    IddDisplayWindow *o = (__bridge IddDisplayWindow *)u;
    uint32_t cap = buf->mAudioDataBytesCapacity;
    uint32_t got = [o pcmPull:(uint8_t *)buf->mAudioData len:cap];
    if (got < cap) memset((uint8_t *)buf->mAudioData + got, 0, cap - got);
    buf->mAudioDataByteSize = cap;
    AudioQueueEnqueueBuffer(aq, buf, 0, NULL);
}

/* Blocking write of exactly len bytes to the transport fd (peer of idd_rd_full). NO on error. */
int idd_wr_full(int fd, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, (size_t)(len - off), 0);
        if (n <= 0) return 0;
        off += (int)n;
    }
    return 1;
}

/* ---- Clipboard format converters (CF_DIB <-> PNG, CF_HTML wrapper, etc.). ---- */
/* Windows CF_HTML <-> bare HTML fragment (NSPasteboardTypeHTML). */
static NSData *idd_cfhtml_to_html(const uint8_t *d, uint32_t n)
{
    NSString *s = [[NSString alloc] initWithBytes:d length:n encoding:NSUTF8StringEncoding];
    if (!s) return nil;
    NSRange sf = [s rangeOfString:@"StartFragment:"], ef = [s rangeOfString:@"EndFragment:"];
    if (sf.location != NSNotFound && ef.location != NSNotFound &&
        sf.location + 24 <= s.length && ef.location + 22 <= s.length) {   /* keep substringWithRange in bounds */
        int so = [[s substringWithRange:NSMakeRange(sf.location + 14, 10)] intValue];
        int eo = [[s substringWithRange:NSMakeRange(ef.location + 12, 10)] intValue];
        if (so >= 0 && eo > so && eo <= (int)n) return [NSData dataWithBytes:d + so length:eo - so];
    }
    return [s dataUsingEncoding:NSUTF8StringEncoding];
}
static NSData *idd_html_to_cfhtml(NSData *frag)
{
    NSString *pre = @"<html><body><!--StartFragment-->", *post = @"<!--EndFragment--></body></html>";
    NSString *hf = @"Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n";
    NSUInteger hl = [[NSString stringWithFormat:hf, 0, 0, 0, 0] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSUInteger pl = [pre lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSUInteger fl = frag.length, ptl = [post lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSString *hdr = [NSString stringWithFormat:hf, (int)hl, (int)(hl + pl + fl + ptl), (int)(hl + pl), (int)(hl + pl + fl)];
    NSMutableData *out = [NSMutableData data];
    [out appendData:[hdr dataUsingEncoding:NSUTF8StringEncoding]];
    [out appendData:[pre dataUsingEncoding:NSUTF8StringEncoding]];
    [out appendData:frag];
    [out appendData:[post dataUsingEncoding:NSUTF8StringEncoding]];
    return out;
}
/* CF_DIB (BITMAPINFOHEADER + bits, no file header) <-> PNG. */
static NSData *idd_dib_to_png(const uint8_t *d, uint32_t n)
{
    if (n < 40) return nil;
    uint32_t biSize = *(const uint32_t *)d, bpp = (n >= 16) ? *(const uint16_t *)(d + 14) : 32;
    uint32_t clrUsed = (n >= 36) ? *(const uint32_t *)(d + 32) : 0;
    uint32_t palette = (bpp <= 8) ? (clrUsed ? clrUsed : (1u << bpp)) * 4 : 0;
    uint32_t off = 14 + biSize + palette;
    NSMutableData *bmp = [NSMutableData dataWithCapacity:14 + n];
    uint8_t fh[14] = { 'B', 'M' }; uint32_t fsz = 14 + n;
    memcpy(fh + 2, &fsz, 4); memcpy(fh + 10, &off, 4);
    [bmp appendBytes:fh length:14]; [bmp appendBytes:d length:n];
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:bmp];
    return rep ? [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
}
static NSData *idd_image_to_dib(NSData *img)
{
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:img];
    if (!rep) return nil;
    NSData *bmp = [rep representationUsingType:NSBitmapImageFileTypeBMP properties:@{}];
    return (bmp.length > 14) ? [bmp subdataWithRange:NSMakeRange(14, bmp.length - 14)] : nil;
}

/* Blocking read of exactly len bytes from the transport fd. Returns NO on EOF/error. The fd is the
   blocking AF_UNIX end the pump bridges to the ch2 g2h ring. */
int idd_rd_full(int fd, void *buf, int len)
{
    uint8_t *p = (uint8_t *)buf;
    int got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, (size_t)(len - got), 0);
        if (n <= 0) return 0;   /* EOF or error -> caller reconnects */
        got += (int)n;
    }
    return 1;
}

/* Aspect-preserving fit (letterbox/pillarbox) of a frameW x frameH frame inside a viewW x viewH view:
   scale = min(viewW/frameW, viewH/frameH), centered. Direct port of compute_letterbox in
   src/backend_win/vm_display_idd.c so the Mac SCALES (never stretches) exactly like Windows. Returns the
   destination rect in the view's (y-up) coordinate space; *outScale (optional) receives the uniform
   scale. -drawRect: and -recordMove: BOTH call this, so render and input can never disagree — the same
   coupling Windows gets by sharing compute_letterbox between d3d_render_frame and window_to_vm_coords. */
static CGRect idd_letterbox(double viewW, double viewH, double frameW, double frameH, double *outScale)
{
    if (viewW <= 0 || viewH <= 0 || frameW <= 0 || frameH <= 0) {
        if (outScale) *outScale = 1.0;
        return CGRectZero;
    }
    double sx = viewW / frameW, sy = viewH / frameH;
    double s = sx < sy ? sx : sy;            /* smaller axis wins -> fit inside, not fill */
    double w = frameW * s, h = frameH * s;
    if (outScale) *outScale = s;
    return CGRectMake((viewW - w) * 0.5, (viewH - h) * 0.5, w, h);   /* centered */
}

/* ================================================================================
 * IddDisplayView -- the frame + input NSView.
 * ================================================================================ */
@implementation IddDisplayView

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

/* The frame is rendered by Metal into this view's CAMetalLayer backing layer (see IddDisplayWindow's
   renderMetal); there is no CPU drawRect for the frame. */
- (instancetype)initWithFrame:(NSRect)f
{
    self = [super initWithFrame:f];
    if (self) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;   /* we drive the layer ourselves */
    }
    return self;
}
- (CALayer *)makeBackingLayer
{
    CAMetalLayer *l = [CAMetalLayer layer];
    l.opaque = YES;
    return l;
}
/* Keep the Metal layer's contentsScale in step with the display (retina) so drawableSize == physical px. */
- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    CGFloat s = self.window.backingScaleFactor;
    if (s > 0) self.layer.contentsScale = s;
}
/* Redraw synchronously on every resize step (incl. live resize) so the frame tracks the window size
   instead of CA stretching the last drawable until the drag ends — matching how Windows repaints inside
   its resize loop. renderMetal sees the new bounds, resizes the drawable, and (during live resize)
   presents in-transaction. */
- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    [self.owner renderMetal];
}
- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    [self.owner renderMetal];   /* final crisp frame at the settled size */
}

- (void)updateTrackingAreas
{
    if (self.track) [self removeTrackingArea:self.track];
    self.track = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self userInfo:nil];
    [self addTrackingArea:self.track];
    [super updateTrackingAreas];
}

/* Apply the guest HW cursor as this view's NSCursor when its shape/visibility changes. */
- (void)updateGuestCursor
{
    IddDisplayWindow *o = self.owner;
    if (!o) return;
    uint32_t fbW = [o frameWidth], fbH = [o frameHeight];
    double scale = 1.0;
    if (fbW && fbH) idd_letterbox(self.bounds.size.width, self.bounds.size.height, fbW, fbH, &scale);
    NSCursor *nc = [o currentCursorForScale:scale];
    if (nc) [self.window invalidateCursorRectsForView:self];
}
- (void)resetCursorRects
{
    NSCursor *c = [self.owner appliedCursor];
    if (c) [self addCursorRect:self.bounds cursor:c];
}

/* No -drawRect: the frame is drawn by Metal into the CAMetalLayer (renderMetal). The HW cursor is applied
   as the macOS NSCursor via updateGuestCursor, so the OS renders it at the real pointer position. */

/* ---- mouse ---- */
- (void)recordMove:(NSEvent *)e
{
    IddDisplayWindow *o = self.owner;
    if (!o) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    if (b.size.width < 1 || b.size.height < 1) return;
    uint32_t gw = [o frameWidth]  ? [o frameWidth]  : 1920;
    uint32_t gh = [o frameHeight] ? [o frameHeight] : 1080;
    /* Invert the SAME letterbox the renderer uses (mirrors window_to_vm_coords): map the click into the
       letterboxed rect, not the whole view, so the cursor lands on the correct VM pixel and a click in a
       bar clamps to the frame edge (Windows behavior). */
    CGRect lb = idd_letterbox(b.size.width, b.size.height, gw, gh, NULL);
    if (lb.size.width < 1 || lb.size.height < 1) return;
    double nx = (p.x - lb.origin.x) / lb.size.width;
    double ny = (lb.origin.y + lb.size.height - p.y) / lb.size.height;   /* y-up view -> top-down guest */
    if (nx < 0) nx = 0; if (nx > 1) nx = 1;
    if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    [o recordMoveX:(uint32_t)(nx * (gw - 1)) y:(uint32_t)(ny * (gh - 1))];
}
- (void)mouseMoved:(NSEvent *)e        { [self recordMove:e]; }
- (void)mouseDragged:(NSEvent *)e      { [self recordMove:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self recordMove:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self recordMove:e]; }
- (void)mouseDown:(NSEvent *)e  { [self recordMove:e]; [self.owner flushMove]; [self.owner sendInput:INPUT_MOUSE_BUTTON p1:INPUT_BTN_LEFT p2:1 p3:0]; }
- (void)mouseUp:(NSEvent *)e    { (void)e; [self.owner flushMove]; [self.owner sendInput:INPUT_MOUSE_BUTTON p1:INPUT_BTN_LEFT p2:0 p3:0]; }
- (void)rightMouseDown:(NSEvent *)e { [self recordMove:e]; [self.owner flushMove]; [self.owner sendInput:INPUT_MOUSE_BUTTON p1:INPUT_BTN_RIGHT p2:1 p3:0]; }
- (void)rightMouseUp:(NSEvent *)e   { (void)e; [self.owner flushMove]; [self.owner sendInput:INPUT_MOUSE_BUTTON p1:INPUT_BTN_RIGHT p2:0 p3:0]; }
- (void)otherMouseDown:(NSEvent *)e { if (e.buttonNumber == 2) { [self recordMove:e]; [self.owner flushMove]; [self.owner sendInput:INPUT_MOUSE_BUTTON p1:INPUT_BTN_MIDDLE p2:1 p3:0]; } }
- (void)otherMouseUp:(NSEvent *)e   { if (e.buttonNumber == 2) { [self.owner flushMove]; [self.owner sendInput:INPUT_MOUSE_BUTTON p1:INPUT_BTN_MIDDLE p2:0 p3:0]; } }
- (void)scrollWheel:(NSEvent *)e
{
    [self.owner flushMove];
    double dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY : e.scrollingDeltaY * 10.0;
    int32_t delta = (int32_t)(dy * 12.0);
    if (delta != 0) [self.owner sendInput:INPUT_MOUSE_WHEEL p1:(uint32_t)delta p2:0 p3:0];
}

/* ---- keyboard ---- */
- (void)keyDown:(NSEvent *)e
{
    [self.owner flushMove];
    unsigned short kc = e.keyCode;
    uint8_t vk = (kc < 128) ? g_vk[kc] : 0;
    if (vk) [self.owner sendInput:INPUT_KEY p1:vk p2:0 p3:(is_extended(kc) ? 1 : 0)];
    /* swallow (no super) to avoid the system beep */
}
- (void)keyUp:(NSEvent *)e
{
    [self.owner flushMove];
    unsigned short kc = e.keyCode;
    uint8_t vk = (kc < 128) ? g_vk[kc] : 0;
    if (vk) [self.owner sendInput:INPUT_KEY p1:vk p2:0 p3:((is_extended(kc) ? 1 : 0) | 2)];
}
- (void)flagsChanged:(NSEvent *)e
{
    [self.owner flushMove];
    unsigned short kc = e.keyCode;
    uint32_t vk = 0, mask = 0;
    switch (kc) {
    case 0x38: case 0x3C: vk = 0x10; mask = NSEventModifierFlagShift;    break; /* Shift */
    case 0x3B: case 0x3E: vk = 0x11; mask = NSEventModifierFlagControl;  break; /* Control */
    case 0x3A: case 0x3D: vk = 0x12; mask = NSEventModifierFlagOption;   break; /* Option->Alt */
    case 0x37:            vk = 0x5B; mask = NSEventModifierFlagCommand;  break; /* LCmd->LWin */
    case 0x36:            vk = 0x5C; mask = NSEventModifierFlagCommand;  break; /* RCmd->RWin */
    case 0x39:            vk = 0x14; mask = NSEventModifierFlagCapsLock; break; /* CapsLock */
    default: return;
    }
    BOOL down = (e.modifierFlags & mask) != 0;
    [self.owner sendInput:INPUT_KEY p1:vk p2:0 p3:(down ? 0 : 2)];
}
@end
