/*
 * appsandbox-display.c — Linux frame capture for AppSandbox.
 *
 * Listens on AF_VSOCK port 2, captures the primary KMS framebuffer via
 * libdrm (drmModeGetFB2 + drmPrimeHandleToFD + mmap), and ships frames
 * in the ASFR wire format the Windows host already understands.
 *
 * No PipeWire, no X11 capture — one layer deeper, straight off KMS.
 *
 * Requires CAP_SYS_ADMIN to call drmModeGetFB2 (since kernel 4.15).
 * The systemd unit runs the binary as root, which satisfies that.
 *
 * Build:  gcc -O2 -Wall -o appsandbox-display appsandbox-display.c -ldrm
 *
 * Wire protocol reference: src/backend_win/vm_display_idd.c (FRAME_MAGIC
 * 'ASFR', packed FrameHeader { magic, w, h, stride, frame_seq, rect_cnt },
 * then rect_cnt RECT(int32 ltrb), then u32 data_size, then BGRA bytes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <systemd/sd-daemon.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <linux/vm_sockets.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "display-gpu.h"
#include "asb_capture_socket.h"
#include "asb_drm_uapi.h"

#define VSOCK_PORT      2
#define FRAME_MAGIC     0x52465341u   /* 'ASFR' little-endian */
#define CURSOR_MAGIC    0x52435341u   /* 'ASCR' little-endian */
#define TARGET_FPS      60
#define FRAME_INTERVAL_NS (1000000000L / TARGET_FPS)

#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

#pragma pack(push, 1)
struct frame_header {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t frame_seq;
    uint32_t dirty_rect_count;
};

struct cursor_header {
    uint32_t magic;            /* CURSOR_MAGIC */
    int32_t  x, y;             /* CRTC coordinates (can be negative) */
    uint32_t visible;          /* 1 if a fb is attached, else 0 */
    uint32_t shape_updated;    /* 1 if bitmap follows */
    uint32_t shape_id;         /* monotonic — host caches by id */
    uint32_t width, height, pitch;
    uint32_t xhot, yhot;
    uint32_t cursor_type;      /* CURSOR_TYPE_* */
    uint32_t shape_data_size;  /* bytes of bitmap after header */
};
#pragma pack(pop)

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void agent_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[display] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- vsock listener ---- */

static int vsock_listen(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { agent_log("socket: %s", strerror(errno)); return -1; }

    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("bind :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, port==ASB_CH_SCREENSHOT?8:1) < 0) {
        agent_log("listen: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static ssize_t send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* ---- DRM capture ---- */

struct capture_ctx {
    int fd;
    uint32_t fb_id_last;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *mem;        /* mmapped framebuffer (read-only) */
    size_t   mem_size;
    int      dma_fd;
};

static void drm_release_fb(struct capture_ctx *c)
{
    if (c->mem && c->mem != MAP_FAILED) {
        munmap(c->mem, c->mem_size);
        c->mem = NULL; c->mem_size = 0;
    }
    if (c->dma_fd >= 0) { close(c->dma_fd); c->dma_fd = -1; }
    c->fb_id_last = 0;
    c->width = c->height = c->stride = 0;
}

static void drm_free_fb(int fd, drmModeFB2 *fb)
{
    if (!fb) return;
    for (unsigned i = 0; i < 4; ++i) {
        if (!fb->handles[i]) continue;
        unsigned j;
        for (j = 0; j < i; ++j)
            if (fb->handles[j] == fb->handles[i]) break;
        if (j == i) {
            struct drm_gem_close close_handle = {.handle = fb->handles[i]};
            ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
        }
    }
    drmModeFreeFB2(fb);
}

/* Find the active primary-plane framebuffer and map it for read.
 * Re-maps only when the fb_id changes (compositor flipped to a new buffer). */
static int drm_acquire_fb(struct capture_ctx *c)
{
    int rc = -1;
    drmModePlaneRes *pres = drmModeGetPlaneResources(c->fd);
    if (!pres) return -1;

    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(c->fd, pres->planes[i]);
        if (!p) continue;
        if (p->fb_id == 0 || p->crtc_id == 0) {
            drmModeFreePlane(p);
            continue;
        }

        /* Skip non-PRIMARY planes — cursor / overlay planes also carry
         * a fb but they're small / partial. We want the desktop. */
        {
            drmModeObjectProperties *props = drmModeObjectGetProperties(
                c->fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            int is_primary = 0;
            if (props) {
                for (uint32_t k = 0; k < props->count_props; k++) {
                    drmModePropertyRes *pr = drmModeGetProperty(c->fd, props->props[k]);
                    if (!pr) continue;
                    if (strcmp(pr->name, "type") == 0 &&
                        props->prop_values[k] == DRM_PLANE_TYPE_PRIMARY)
                        is_primary = 1;
                    drmModeFreeProperty(pr);
                    if (is_primary) break;
                }
                drmModeFreeObjectProperties(props);
            }
            if (!is_primary) {
                drmModeFreePlane(p);
                continue;
            }
        }

        /* Same fb as last frame? Reuse the existing mapping. */
        if (p->fb_id == c->fb_id_last && c->mem) {
            drmModeFreePlane(p);
            rc = 0;
            goto out;
        }

        uint32_t fb_id = p->fb_id;
        drmModeFB2 *fb2 = drmModeGetFB2(c->fd, fb_id);
        drmModeFreePlane(p);
        if (!fb2) continue;

        /* Only XRGB8888 / ARGB8888 for now — that's what hyperv_drm uses. */
        if (fb2->pixel_format != DRM_FORMAT_XRGB8888 &&
            fb2->pixel_format != DRM_FORMAT_ARGB8888) {
            drm_free_fb(c->fd, fb2);
            continue;
        }

        uint32_t handle = fb2->handles[0];
        uint32_t pitch  = fb2->pitches[0];
        uint32_t height = fb2->height;
        uint32_t width  = fb2->width;
        if (!handle || !pitch || !height) {
            drm_free_fb(c->fd, fb2);
            continue;
        }

        int dma_fd = -1;
        if (drmPrimeHandleToFD(c->fd, handle, DRM_CLOEXEC | O_RDONLY, &dma_fd) < 0) {
            drm_free_fb(c->fd, fb2);
            continue;
        }
        drm_free_fb(c->fd, fb2);
        size_t size = (size_t)pitch * height;
        void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_fd, 0);
        if (m == MAP_FAILED) {
            close(dma_fd);
            continue;
        }

        drm_release_fb(c);
        c->fb_id_last = fb_id;
        c->width  = width;
        c->height = height;
        c->stride = pitch;
        c->mem      = (uint8_t *)m;
        c->mem_size = size;
        c->dma_fd   = dma_fd;
        rc = 0;
        goto out;
    }

out:
    drmModeFreePlaneResources(pres);
    return rc;
}

struct screenshot_capture {
    AsbCaptureSocket *service;
    struct capture_ctx cpu;
    struct display_gpu_capture *gpu;
    uint64_t sequence;
};

static uint64_t screenshot_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

static uint32_t screenshot_capture_frame(void *context,const AsbScreenshotRequest *request,
    AsbScreenshotReply *reply,unsigned char **output)
{
    struct screenshot_capture *capture=context;
    uint64_t deadline=screenshot_time()+request->timeout_ms;
    uint32_t error=1460;
    display_gpu_capture_begin(capture->gpu);
    while (!g_stop && screenshot_time()<deadline) {
        const void *pixels=NULL;
        uint32_t width=0,height=0,stride=0;
        uint64_t sequence=0;
        int shared=0;
        int result=display_gpu_capture_poll(capture->gpu,&pixels,&width,&height,&stride,&sequence,&shared);
        if (result<0) { error=21;break; }
        if (result==2) {
            if (drm_acquire_fb(&capture->cpu)==0) {
                pixels=capture->cpu.mem;width=capture->cpu.width;height=capture->cpu.height;
                stride=capture->cpu.stride;sequence=++capture->sequence;result=1;
            } else result=0;
        }
        if (result==1) {
            reply->width=width;reply->height=height;
            unsigned char *copy=NULL;
            error=asb_capture_pack(pixels,stride,reply,&copy);
            if (error) break;
            if (!display_gpu_capture_valid(capture->gpu)) {
                free(copy);display_gpu_capture_begin(capture->gpu);continue;
            }
            reply->frame_seq=sequence;
            reply->capture_time_ms=screenshot_time();reply->flags=shared?ASB_SCREENSHOT_GPU_SHARED:0;
            *output=copy;error=0;break;
        }
        struct timespec delay={.tv_nsec=2000000};
        nanosleep(&delay,NULL);
    }
    display_gpu_capture_end(capture->gpu);
    return g_stop?995:error;
}

/* ---- Cursor plane: discover + emit ASCR on change ----
 *
 * Mutter updates the cursor plane independently of the primary plane,
 * so the host sees smooth pointer movement even at our TARGET_FPS primary
 * cadence. We cache the plane id + the CRTC_X / CRTC_Y / HOTSPOT_X /
 * HOTSPOT_Y property ids once at startup, then on every tick read the
 * current values via drmModeObjectGetProperties (one ioctl, ~µs cost). */

struct cursor_state {
    int       discovered;
    uint32_t  plane_id;
    uint32_t  prop_crtc_x, prop_crtc_y;
    uint32_t  prop_hotspot_x, prop_hotspot_y;

    /* Last sent values — only re-send when something changes. */
    uint32_t  last_fb_id;
    int32_t   last_x, last_y;
    uint32_t  last_hotspot_x, last_hotspot_y;
    uint32_t  shape_id;
};

static void cursor_init(int fd, struct cursor_state *cur)
{
    drmModePlaneRes *pres;
    memset(cur, 0, sizeof(*cur));

    pres = drmModeGetPlaneResources(fd);
    if (!pres) return;

    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
        if (!p) continue;
        drmModeObjectProperties *props = drmModeObjectGetProperties(
            fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        int is_cursor = 0;
        uint32_t cx = 0, cy = 0, hx = 0, hy = 0;
        if (props) {
            for (uint32_t k = 0; k < props->count_props; k++) {
                drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[k]);
                if (!pr) continue;
                if (strcmp(pr->name, "type") == 0 &&
                    props->prop_values[k] == DRM_PLANE_TYPE_CURSOR)
                    is_cursor = 1;
                else if (strcmp(pr->name, "CRTC_X") == 0)    cx = pr->prop_id;
                else if (strcmp(pr->name, "CRTC_Y") == 0)    cy = pr->prop_id;
                else if (strcmp(pr->name, "HOTSPOT_X") == 0) hx = pr->prop_id;
                else if (strcmp(pr->name, "HOTSPOT_Y") == 0) hy = pr->prop_id;
                drmModeFreeProperty(pr);
            }
            drmModeFreeObjectProperties(props);
        }
        if (is_cursor) {
            cur->discovered     = 1;
            cur->plane_id       = p->plane_id;
            cur->prop_crtc_x    = cx;
            cur->prop_crtc_y    = cy;
            cur->prop_hotspot_x = hx;
            cur->prop_hotspot_y = hy;
            drmModeFreePlane(p);
            /* Log the cached cur->plane_id, not p->plane_id: p was just freed
               above, so reading p->plane_id here was a use-after-free (L12). */
            agent_log("cursor plane %u (CRTC_X=%u CRTC_Y=%u HOTSPOT_X=%u HOTSPOT_Y=%u)",
                      cur->plane_id, cx, cy, hx, hy);
            break;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pres);
    if (!cur->discovered)
        agent_log("no cursor plane found — host will see no smooth cursor");
}

/* Read a property value from the cached object-properties blob. */
static uint64_t prop_get(drmModeObjectProperties *props, uint32_t prop_id)
{
    if (!prop_id || !props) return 0;
    for (uint32_t k = 0; k < props->count_props; k++)
        if (props->props[k] == prop_id)
            return props->prop_values[k];
    return 0;
}

/* Sign-extend CRTC_X / CRTC_Y: kernel stores them as u64 but they're
 * semantically int32_t (cursor can be off-screen). */
static int32_t sext32(uint64_t v) { return (int32_t)(uint32_t)v; }

static int cursor_tick(int client_fd, int drm_fd, struct cursor_state *cur)
{
    if (!cur->discovered) return 0;

    drmModePlane *p = drmModeGetPlane(drm_fd, cur->plane_id);
    if (!p) return 0;
    uint32_t fb_id = p->fb_id;
    drmModeFreePlane(p);

    drmModeObjectProperties *props = drmModeObjectGetProperties(
        drm_fd, cur->plane_id, DRM_MODE_OBJECT_PLANE);
    int32_t  cx  = sext32(prop_get(props, cur->prop_crtc_x));
    int32_t  cy  = sext32(prop_get(props, cur->prop_crtc_y));
    uint32_t hx  = (uint32_t)prop_get(props, cur->prop_hotspot_x);
    uint32_t hy  = (uint32_t)prop_get(props, cur->prop_hotspot_y);
    if (props) drmModeFreeObjectProperties(props);

    int pos_changed   = (cx != cur->last_x) || (cy != cur->last_y) ||
                        (hx != cur->last_hotspot_x) || (hy != cur->last_hotspot_y);
    int shape_changed = (fb_id != cur->last_fb_id);

    if (!pos_changed && !shape_changed) return 0;

    /* If shape changed, read the cursor fb shape bytes via PRIME. */
    drmModeFB2 *fb2 = NULL;
    int     dma_fd = -1;
    void   *bmp    = NULL;
    size_t  bmp_sz = 0;
    uint32_t fb_w = 0, fb_h = 0, fb_pitch = 0;

    if (shape_changed && fb_id != 0) {
        fb2 = drmModeGetFB2(drm_fd, fb_id);
        if (fb2 && fb2->handles[0]) {
            if (drmPrimeHandleToFD(drm_fd, fb2->handles[0],
                                   DRM_CLOEXEC | O_RDONLY, &dma_fd) == 0) {
                fb_w     = fb2->width;
                fb_h     = fb2->height;
                fb_pitch = fb2->pitches[0];
                bmp_sz   = (size_t)fb_pitch * fb_h;
                bmp = mmap(NULL, bmp_sz, PROT_READ, MAP_SHARED, dma_fd, 0);
                if (bmp == MAP_FAILED) { bmp = NULL; bmp_sz = 0; }
            }
        }
    }

    /* Crop the cursor bitmap to its non-transparent bounding box, then
     * find the artist's hotspot heuristically as the topmost-leftmost
     * pixel with "really opaque" alpha (>= HOT_ALPHA_THRESHOLD). The
     * full bbox includes anti-aliased edge pixels that fade in with
     * alpha 1..127 before the cursor body starts; those aren't the
     * cursor tip, just edge feathering. The first pixel above the
     * threshold approximates where the artist would have placed the
     * hotspot for arrow / hand / crosshair cursors (it'd be a few
     * pixels off for I-beam / centered cursors, but those are uncommon
     * over the IDD window).
     *
     * Mutter doesn't set HOTSPOT_X/HOTSPOT_Y on us (asb_drm isn't in
     * Mutter's hotspot-driver allowlist), so we have to recover the
     * hotspot from the bitmap. Mutter has already pre-adjusted CRTC_X
     * by the hotspot, but the host ignores ASCR x/y anyway — it uses
     * the OS pointer position and renders the HCURSOR with the hotspot
     * we provide.
     *
     * Alpha stays premultiplied: the host's create_cursor_from_data
     * (vm_display_idd.c, CURSOR_TYPE_ALPHA branch) explicitly expects
     * 32bpp BGRA with premultiplied alpha. */
    #define HOT_ALPHA_THRESHOLD  128
    uint8_t  *cropped     = NULL;
    uint32_t  out_w       = 0, out_h = 0, out_pitch = 0;
    size_t    out_sz      = 0;
    uint32_t  hot_x_in_crop = 0, hot_y_in_crop = 0;

    if (shape_changed && bmp && bmp_sz > 0 && fb_pitch > 0) {
        const uint8_t *src = (const uint8_t *)bmp;
        uint32_t min_x = fb_w, min_y = fb_h, max_x = 0, max_y = 0;
        uint32_t hot_x = 0, hot_y = 0;
        int any = 0, hot_found = 0;
        for (uint32_t y = 0; y < fb_h; y++) {
            const uint8_t *row = src + (size_t)y * fb_pitch;
            for (uint32_t x = 0; x < fb_w; x++) {
                uint8_t a = row[x * 4 + 3];
                if (a != 0) {
                    if (!any) { min_x = max_x = x; min_y = max_y = y; any = 1; }
                    else {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                    }
                    if (!hot_found && a >= HOT_ALPHA_THRESHOLD) {
                        hot_x = x; hot_y = y; hot_found = 1;
                    }
                }
            }
        }
        if (any) {
            out_w     = max_x - min_x + 1;
            out_h     = max_y - min_y + 1;
            out_pitch = out_w * 4;
            out_sz    = (size_t)out_pitch * out_h;
            cropped   = (uint8_t *)malloc(out_sz);
            if (cropped) {
                /* Pass Mutter's premultiplied bytes through unchanged. */
                for (uint32_t y = 0; y < out_h; y++) {
                    memcpy(cropped + (size_t)y * out_pitch,
                           src + ((size_t)(y + min_y) * fb_pitch) + (size_t)min_x * 4,
                           out_pitch);
                }
                if (hot_found) {
                    hot_x_in_crop = hot_x - min_x;
                    hot_y_in_crop = hot_y - min_y;
                }
            } else {
                out_sz = 0;
            }
        }
    }

    struct cursor_header h;
    memset(&h, 0, sizeof(h));
    h.magic         = CURSOR_MAGIC;
    h.x             = cx;
    h.y             = cy;
    h.visible       = (fb_id != 0) ? 1 : 0;
    h.shape_updated = (cropped && out_sz > 0) ? 1 : 0;
    h.shape_id      = cur->shape_id + (h.shape_updated ? 1 : 0);
    h.width         = out_w;
    h.height        = out_h;
    h.pitch         = out_pitch;
    h.xhot          = hot_x_in_crop;
    h.yhot          = hot_y_in_crop;
    h.cursor_type   = CURSOR_TYPE_ALPHA;
    h.shape_data_size = h.shape_updated ? (uint32_t)out_sz : 0;

    int rc = 0;
    if (send_all(client_fd, &h, sizeof(h)) < 0) rc = -1;
    else if (h.shape_updated &&
             send_all(client_fd, cropped, out_sz) < 0) rc = -1;

    if (cropped) free(cropped);

    if (bmp)    munmap(bmp, bmp_sz);
    if (dma_fd >= 0) close(dma_fd);
    drm_free_fb(drm_fd, fb2);

    if (rc == 0) {
        cur->last_fb_id      = fb_id;
        cur->last_x          = cx;
        cur->last_y          = cy;
        cur->last_hotspot_x  = hx;
        cur->last_hotspot_y  = hy;
        if (h.shape_updated) cur->shape_id = h.shape_id;
    }
    return rc;
}

/* ---- Main capture loop ---- */

static int send_frame(int client_fd, struct capture_ctx *c, uint64_t seq)
{
    struct frame_header h;
    h.magic            = FRAME_MAGIC;
    h.width            = c->width;
    h.height           = c->height;
    h.stride           = c->stride;
    h.frame_seq        = seq;
    h.dirty_rect_count = 0;

    if (send_all(client_fd, &h, sizeof(h)) < 0) return -1;

    uint32_t data_size = c->stride * c->height;
    if (send_all(client_fd, &data_size, sizeof(data_size)) < 0) return -1;

    if (data_size > 0) {
        if (send_all(client_fd, c->mem, data_size) < 0) return -1;
    }
    return 0;
}

static int capture_open(void)
{
    struct capture_ctx ctx = { .fd = -1, .dma_fd = -1 };

    /* Find a /dev/dri/cardN by DRM driver name, in preference order:
     *   asb_drm     — our custom virtual display driver (preferred)
     *   hyperv_drm  — Hyper-V synthetic GPU (fallback for VMs without asb_drm)
     *
     * Matching on driver name inherently skips the boot framebuffers
     * (simpledrm/efifb/vesafb) that Mutter isn't rendering to. The legacy
     * fallback below (used only if neither driver is found) is the path
     * that filters on a connected connector instead. */
    static const char *DRIVER_PRIORITY[] = {
        "asb_drm",
        "hyperv_drm",
        NULL,
    };

    int ctx_fd = -1;
    char chosen_path[64] = {0};
    for (int prio = 0; DRIVER_PRIORITY[prio] && ctx_fd < 0; prio++) {
        for (int i = 0; i < 8 && ctx_fd < 0; i++) {
            char path[64], uevent_path[96], uevent[256];
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);
            snprintf(uevent_path, sizeof(uevent_path),
                     "/sys/class/drm/card%d/device/uevent", i);
            FILE *uf = fopen(uevent_path, "r");
            if (!uf) continue;
            int match = 0;
            while (fgets(uevent, sizeof(uevent), uf)) {
                if (strncmp(uevent, "DRIVER=", 7) != 0) continue;
                char *nl = strchr(uevent + 7, '\n');
                if (nl) *nl = '\0';
                if (strcmp(uevent + 7, DRIVER_PRIORITY[prio]) == 0)
                    match = 1;
                break;
            }
            fclose(uf);
            if (!match) continue;
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            ctx_fd = fd;
            snprintf(chosen_path, sizeof(chosen_path), "%s", path);
            agent_log("selected %s (%s)", path, DRIVER_PRIORITY[prio]);
        }
    }
    if (ctx_fd >= 0) {
        ctx.fd = ctx_fd;
        agent_log("using %s", chosen_path);
        /* Skip the legacy fallback enumeration below. */
        goto have_card;
    }

    /* Legacy fallback: scan unfiltered, skip boot framebuffers. */
    for (int i = 0; i < 8; i++) {
        char path[64], uevent_path[96], uevent[256];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        snprintf(uevent_path, sizeof(uevent_path),
                 "/sys/class/drm/card%d/device/uevent", i);
        FILE *uf = fopen(uevent_path, "r");
        if (uf) {
            int boot_fb = 0;
            while (fgets(uevent, sizeof(uevent), uf)) {
                if (strncmp(uevent, "DRIVER=", 7) != 0) continue;
                const char *drv = uevent + 7;
                if (strstr(drv, "simple-framebuffer") ||
                    strstr(drv, "simpledrm") ||
                    strstr(drv, "efifb") ||
                    strstr(drv, "vesafb")) {
                    boot_fb = 1;
                }
                break;
            }
            fclose(uf);
            if (boot_fb) {
                agent_log("skipping %s (boot framebuffer)", path);
                continue;
            }
        }
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        /* The first userspace opener of /dev/dri/cardN becomes the
         * implicit DRM master. If we hold master, Mutter/gnome-shell
         * later gets EBUSY when it tries to claim master and the
         * compositor never starts. Drop master immediately; we only
         * need read access to the framebuffer (via drmModeGetFB2 +
         * drmPrimeHandleToFD, both of which want CAP_SYS_ADMIN, not
         * master). EACCES here just means we weren't master to start
         * with — fine, nothing to drop. */
        drmDropMaster(fd);
        drmModeRes *res = drmModeGetResources(fd);
        if (res) {
            int has_conn = 0;
            for (int k = 0; k < res->count_connectors; k++) {
                drmModeConnector *con = drmModeGetConnector(fd, res->connectors[k]);
                if (con) {
                    if (con->connection == DRM_MODE_CONNECTED) has_conn = 1;
                    drmModeFreeConnector(con);
                    if (has_conn) break;
                }
            }
            drmModeFreeResources(res);
            if (has_conn) {
                ctx.fd = fd;
                agent_log("using %s", path);
                break;
            }
        }
        close(fd);
    }
    if (ctx.fd < 0) {
        agent_log("no /dev/dri/cardN with a connected output");
        return -1;
    }

have_card:
    /* Drop implicit DRM master we may have picked up by being first to
     * open the device — Mutter needs to become master to render. */
    drmDropMaster(ctx.fd);

    /* Universal planes lets us enumerate the primary; atomic exposes the
     * standard property set (CRTC_X / CRTC_Y / FB_ID / type / ...) so we
     * can read cursor position via drmModeObjectGetProperties. Without
     * the atomic cap those property ids come back as 0 and cursor sync
     * silently degrades to "position never changes". */
    drmSetClientCap(ctx.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(ctx.fd, DRM_CLIENT_CAP_ATOMIC, 1);
    /* Drivers that set DRIVER_CURSOR_HOTSPOT (we do, in asb_drm) hide the
     * cursor plane from clients that don't ack hotspot semantics — they'd
     * otherwise paint cursors at the wrong place. Opt in so the daemon
     * actually sees the cursor plane in drmModeGetPlaneResources(). */
#ifdef DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT
    drmSetClientCap(ctx.fd, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1);
#else
    /* libdrm < 2.4.118 doesn't define the constant. Value is 6 per the
     * uapi in <drm/drm.h>; safe to set numerically. */
    drmSetClientCap(ctx.fd, 6, 1);
#endif

    return ctx.fd;
}

static void capture_size(int fd, uint32_t *width, uint32_t *height)
{
    *width = *height = 0;
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources) return;
    for (int i = 0; i < resources->count_crtcs; ++i) {
        drmModeCrtc *crtc = drmModeGetCrtc(fd, resources->crtcs[i]);
        if (!crtc) continue;
        if (crtc->mode_valid && crtc->buffer_id) {
            *width = crtc->width;
            *height = crtc->height;
        }
        drmModeFreeCrtc(crtc);
        if (*width && *height) break;
    }
    drmModeFreeResources(resources);
}

struct display_config {
    int supported;
    struct drm_asb_display_config current, fallback, previous;
    uint64_t reconnect_until, resize_until;
    DisplayGpuPacket request;
};

static uint64_t display_time_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int display_config_set(int fd, struct display_config *config,
                              struct drm_asb_display_config next)
{
    if (!config->supported) return 0;
    if (config->current.flags == next.flags && config->current.width == next.width &&
        config->current.height == next.height && config->current.refresh == next.refresh) return 0;
    next.operation = ASB_DRM_DISPLAY_SET;
    if (drmIoctl(fd, DRM_IOCTL_ASB_DISPLAY_CONFIG, &next) < 0) {
        agent_log("display mode update failed: %s", strerror(errno));
        return -1;
    }
    config->current = next;
    return 0;
}

static void display_config_fallback(int fd, struct display_config *config)
{
    display_config_set(fd, config, config->fallback);
    config->reconnect_until = config->resize_until = 0;
}

static int display_resize_failed(int client_fd, uint64_t session, struct display_config *config)
{
    if (client_fd < 0 || !session || !config->request.status) return 0;
    DisplayGpuPacket reply = config->request;
    reply.session = session;
    reply.type = DISPLAY_GPU_RESIZED;
    if (send_all(client_fd, &reply, sizeof(reply)) < 0) return -1;
    config->request.status = 0;
    return 0;
}

static int display_config_expire(int fd, struct display_config *config, int client_fd,
                                 uint64_t session)
{
    uint64_t now = display_time_ms();
    if (config->reconnect_until && now >= config->reconnect_until) {
        display_config_fallback(fd, config);
        return 1;
    }
    if (config->resize_until && now >= config->resize_until) {
        agent_log("display resize timed out; restoring previous mode");
        config->previous.flags = ASB_DRM_DISPLAY_SHARED;
        display_config_set(fd, config, config->previous);
        config->resize_until = 0;
        config->request.status = ETIMEDOUT;
        display_resize_failed(client_fd, session, config);
        return 1;
    }
    return 0;
}

static int display_resize(int fd, int client_fd, struct display_config *config,
                           const DisplayGpuPacket *request, uint64_t session, int supported,
                           uint32_t width, uint32_t height)
{
    DisplayGpuPacket reply = *request;
    reply.type = DISPLAY_GPU_RESIZED;
    reply.status = EINVAL;
    if (supported && request->session == session && request->slot <= DISPLAY_SIZE_WINDOW &&
        (!request->handle || (config->current.refresh &&
         request->handle >= DISPLAY_MIN_REFRESH && request->handle <= DISPLAY_MAX_REFRESH)) &&
        request->width >= 64 && request->height >= 64 &&
        request->width <= DISPLAY_MAX_WIDTH && request->height <= DISPLAY_MAX_HEIGHT &&
        (request->slot == DISPLAY_SIZE_GUEST ||
         (request->width >= DISPLAY_MIN_WIDTH && request->height >= DISPLAY_MIN_HEIGHT))) {
        struct drm_asb_display_config next = {
            .flags = ASB_DRM_DISPLAY_SHARED, .width = width, .height = height,
            .refresh = config->current.refresh ?
                (request->slot == DISPLAY_SIZE_WINDOW && request->handle ? (uint32_t)request->handle : 60) : 0,
        };
        if (request->slot == DISPLAY_SIZE_WINDOW) {
            next.flags |= ASB_DRM_DISPLAY_MATCH_WINDOW;
            next.width = request->width;
            next.height = request->height;
        }
        struct drm_asb_display_config previous = config->current;
        if (!config->resize_until) {
            previous.width = width;
            previous.height = height;
        }
        if (display_config_set(fd, config, next) == 0) {
            reply.status = 0;
            if (next.width != width || next.height != height) {
                if (!config->resize_until) config->previous = previous;
                config->resize_until = display_time_ms() + 5000;
                config->request = *request;
                config->request.status = 0;
            } else {
                config->resize_until = 0;
            }
        } else {
            reply.status = errno ? (uint32_t)errno : EIO;
        }
    }
    return send_all(client_fd, &reply, sizeof(reply)) < 0 ? -1 : 0;
}

static void capture_loop(int client_fd, int drm_fd, struct display_gpu **gpu,
                          struct display_config *config,struct screenshot_capture *capture)
{
    struct capture_ctx ctx = {.fd = drm_fd, .dma_fd = -1};
    struct cursor_state cur;
    cursor_init(ctx.fd, &cur);
    uint64_t seq = 0;
    DisplayGpuPacket query = {0};
    size_t query_bytes = 0;
    int shared = 0, queried = 0, resize = 0;
    uint64_t session = 0, connected = display_time_ms();
    uint32_t width = 0, height = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!g_stop) {
        if (display_config_expire(drm_fd, config, shared && resize ? client_fd : -1, session) && shared) break;
        if (!shared && *gpu && !display_gpu_healthy(*gpu)) {
            display_gpu_destroy(*gpu);
            *gpu = NULL;
            display_config_fallback(drm_fd, config);
        }
        if (query_bytes < sizeof(query)) {
            ssize_t count = recv(client_fd, (char *)&query + query_bytes,
                                 sizeof(query) - query_bytes, MSG_DONTWAIT);
            if (!count || (count < 0 && errno != EAGAIN && errno != EINTR)) break;
            if (count > 0) query_bytes += (size_t)count;
        } else {
            char byte;
            ssize_t count = recv(client_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (count >= 0 || (errno != EAGAIN && errno != EINTR)) break;
        }
        if (query_bytes == sizeof(query)) {
            if (query.magic != DISPLAY_GPU_MAGIC || query.version != DISPLAY_GPU_VERSION) break;
            if (queried) {
                if (query.type != DISPLAY_GPU_RESIZE ||
                    display_resize(drm_fd, client_fd, config, &query, session, shared && resize,
                                   width, height) < 0) break;
                query_bytes = 0;
            } else if (query.type != DISPLAY_GPU_QUERY) {
                break;
            } else if (!*gpu || display_gpu_active(*gpu)) {
                capture_size(ctx.fd, &width, &height);
                if (*gpu && (!width || !height)) {
                    struct pollfd wait = {.fd = display_gpu_event(*gpu), .events = POLLIN};
                    poll(&wait, 1, 16);
                    continue;
                }
                resize = config->supported && (query.reserved & DISPLAY_GPU_CAP_RESIZE);
                uint32_t size_mode = resize && (config->current.flags & ASB_DRM_DISPLAY_MATCH_WINDOW)
                    ? DISPLAY_SIZE_WINDOW : DISPLAY_SIZE_GUEST;
                shared = display_gpu_connect(*gpu, client_fd, &query, width, height, resize, size_mode,
                                             config->current.refresh);
                if (shared < 0) {
                    shared = 0;
                    display_config_fallback(drm_fd, config);
                    break;
                }
                session = query.session;
                queried = 1;
                query_bytes = 0;
                agent_log("display transport: %s", shared ? "shared GPU textures" : "socket frames");
                if (shared) {
                    struct drm_asb_display_config mode = config->current;
                    if (size_mode == DISPLAY_SIZE_GUEST && mode.flags != ASB_DRM_DISPLAY_SHARED) {
                        mode.flags = ASB_DRM_DISPLAY_SHARED;
                        mode.width = width;
                        mode.height = height;
                    }
                    if (display_config_set(drm_fd, config, mode) < 0) break;
                    if (resize && display_resize_failed(client_fd, session, config) < 0) break;
                    drm_release_fb(&ctx);
                } else {
                    display_config_fallback(drm_fd, config);
                }
            }
        }
        if (shared) {
            int result = display_gpu_frame(*gpu);
            if (result < 0) {
                agent_log("shared GPU session ended (%d)", result);
                if (result == -1) {
                    display_gpu_destroy(*gpu);
                    *gpu = NULL;
                    display_config_fallback(drm_fd, config);
                }
                break;
            }
            if (result > 0) {
                config->reconnect_until = 0;
                if (config->resize_until && width == config->current.width && height == config->current.height)
                    config->resize_until = 0;
            }
        } else {
            if (config->reconnect_until && display_time_ms() - connected >= 1000)
                display_config_fallback(drm_fd, config);
            const void *pixels = NULL;
            struct capture_ctx fallback = {.fd = drm_fd, .dma_fd = -1};
            int result = display_gpu_readback(*gpu, &pixels, &fallback.width,
                                              &fallback.height, &fallback.stride);
            if (result < 0) {
                agent_log("GPU unavailable; returning to software display capture");
                display_gpu_destroy(*gpu);
                *gpu = NULL;
                display_config_fallback(drm_fd, config);
            } else if (result > 0) {
                fallback.mem = (uint8_t *)pixels;
                if (send_frame(client_fd, &fallback, ++seq) < 0) break;
            } else if (drm_acquire_fb(&ctx) == 0) {
                if (send_frame(client_fd, &ctx, ++seq) < 0) break;
            }
        }
        display_gpu_capture_offer(*gpu,capture->gpu,shared);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remaining = (next.tv_sec - now.tv_sec) * 1000000000LL + next.tv_nsec - now.tv_nsec;
        if (remaining <= 0) {
            if (cursor_tick(client_fd, ctx.fd, &cur) < 0) break;
            next = now;
            next.tv_nsec += FRAME_INTERVAL_NS;
            if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
            remaining = FRAME_INTERVAL_NS;
        }
        if (shared) {
            int event = display_gpu_event(*gpu);
            if (event < 0) break;
            struct pollfd wait[] = {{.fd = client_fd, .events = POLLIN}, {.fd = event, .events = POLLIN}};
            if (poll(wait, 2, (int)((remaining + 999999) / 1000000)) < 0 && errno != EINTR) break;
        } else {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
    }
    if (shared && *gpu && !config->reconnect_until)
        config->reconnect_until = display_time_ms() + 10000;
    display_gpu_disconnect(*gpu);
    drm_release_fb(&ctx);
}

static int display_session(int argc, char **argv)
{
    if (argc < 3) return 1;
    int accelerated = 0, queried = 0;
    for (int i = 0; i < 8 && !accelerated; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmDropMaster(fd);
        struct drm_asb_gpu_query query = {0};
        if (ioctl(fd, DRM_IOCTL_ASB_GPU_QUERY, &query) == 0 &&
            query.version == ASB_DRM_GPU_VERSION) {
            queried = 1;
            accelerated |= !!(query.capabilities & ASB_DRM_GPU_CAP_PRESENT);
        }
        close(fd);
    }
    if (!queried) accelerated = access("/run/appsandbox/gpu-present", F_OK) == 0;
    if (accelerated) {
        unsetenv("LIBGL_ALWAYS_SOFTWARE");
        setenv("GALLIUM_DRIVER", "d3d12", 1);
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "d3d12", 1);
        setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1);
    } else {
        unsetenv("GALLIUM_DRIVER");
        unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
        unsetenv("VK_DRIVER_FILES");
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    }
    agent_log("starting compositor with %s rendering", accelerated ? "GPU" : "software");
    execvp(argv[2], &argv[2]);
    return 1;
}

static void display_wait_runtime(void)
{
    if (access("/dev/dxg", F_OK) < 0) return;
    if (mkdir("/run/appsandbox", 0755) < 0 && errno != EEXIST) return;
    int events = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (events < 0) return;
    if (inotify_add_watch(events, "/run/appsandbox", IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(events);
        return;
    }
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!g_stop && access("/run/appsandbox/gpu-ready", F_OK) < 0) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = 15000 - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                     (now.tv_nsec - start.tv_nsec) / 1000000);
        if (remaining <= 0) break;
        struct pollfd event = {.fd = events, .events = POLLIN};
        if (poll(&event, 1, remaining) < 0 && errno != EINTR) break;
        char buffer[4096];
        while (read(events, buffer, sizeof(buffer)) > 0) {}
    }
    close(events);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--session")) return display_session(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    /* Install SIGINT/SIGTERM WITHOUT SA_RESTART so a blocking accept()
     * returns EINTR on signal (the loop below checks for it) instead of
     * auto-restarting. glibc's signal() defaults to BSD SA_RESTART
     * semantics, which would leave accept() blocked through shutdown and
     * make systemd wait the full stop timeout. Mirrors the agent. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    unlink("/run/appsandbox/gpu-present");
    display_wait_runtime();
    int drm_fd = capture_open();
    if (drm_fd < 0) return 1;
    struct display_config config = {0};
    config.supported = drmIoctl(drm_fd, DRM_IOCTL_ASB_DISPLAY_CONFIG, &config.current) == 0;
    config.fallback = config.current;
    config.fallback.flags = 0;
    if (config.fallback.refresh > 60) config.fallback.refresh = 60;
    struct display_gpu *gpu = display_gpu_create(drm_fd);
    agent_log("GPU presenter: %s", gpu ? "ready" : "unavailable");

    int screenshot_listener=vsock_listen(ASB_CH_SCREENSHOT);
    struct screenshot_capture capture={.cpu={.fd=drm_fd,.dma_fd=-1}};
    if (screenshot_listener>=0) {
        capture.gpu=display_gpu_capture_create();
        if (capture.gpu) capture.service=asb_capture_socket_start(screenshot_listener,&capture,screenshot_capture_frame);
        else close(screenshot_listener);
    }
    if (capture.service) agent_log("independent screenshot listener on vsock :%u",ASB_CH_SCREENSHOT);
    else {
        display_gpu_capture_destroy(capture.gpu);capture.gpu=NULL;
        agent_log("Screenshot service unavailable; continuing normal display output.");
    }

    int srv = vsock_listen(VSOCK_PORT);
    if (srv < 0) { asb_capture_socket_stop(capture.service);display_gpu_capture_destroy(capture.gpu);display_gpu_destroy(gpu);close(drm_fd);return 1; }
    agent_log("listening on vsock :%u", VSOCK_PORT);
    sd_notify(0, "READY=1");

    while (!g_stop) {
        if (gpu && !display_gpu_healthy(gpu)) {
            display_gpu_destroy(gpu);
            gpu = NULL;
            display_config_fallback(drm_fd, &config);
        }
        display_config_expire(drm_fd, &config, -1, 0);
        display_gpu_capture_offer(gpu,capture.gpu,-1);
        struct pollfd pending[]={{.fd=srv,.events=POLLIN},
            {.fd=display_gpu_capture_event(capture.gpu),.events=POLLIN}};
        if (poll(pending,2,1000)<=0 || !(pending[0].revents&POLLIN)) continue;
        struct sockaddr_vm peer;
        socklen_t plen = sizeof(peer);
        int c = accept(srv, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("accept: %s", strerror(errno));
            break;
        }
        agent_log("client connected (cid=%u)", peer.svm_cid);
        struct timeval timeout = {.tv_sec = 2};
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        capture_loop(c, drm_fd, &gpu, &config,&capture);
        close(c);
    }

    close(srv);
    asb_capture_socket_stop(capture.service);
    drm_release_fb(&capture.cpu);
    display_gpu_capture_destroy(capture.gpu);
    display_gpu_destroy(gpu);
    close(drm_fd);
    return 0;
}
