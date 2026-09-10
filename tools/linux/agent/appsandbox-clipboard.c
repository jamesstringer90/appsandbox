/*
 * appsandbox-clipboard.c — Linux clipboard sync for AppSandbox.
 *
 * Bridges the GNOME / Wayland clipboard to the host's existing per-VM
 * clipboard module (src/backend_win/vm_clipboard.c) over AF_VSOCK ports
 * 5 (host→guest writer) and 6 (guest→host reader).
 *
 * Wire protocol: V1 ClipHeader-framed, matching what the Windows host
 * already speaks. The host treats every format as opaque bytes keyed by
 * the Windows CF_* ID (or registered-format name string for IDs ≥ 0xC000),
 * so the entire conversion matrix between CF_* and X11 / Wayland targets
 * lives in this daemon.
 *
 * Approach (kernel-userland boundary is wrong here — clipboard exists at
 * the compositor layer):
 *   - One XCB connection to $DISPLAY (XWayland). Mutter's clipboard
 *     manager auto-bridges the X11 CLIPBOARD selection to the Wayland
 *     wl_data_device, so owning CLIPBOARD covers both ecosystems.
 *   - One epoll loop with: two vsock listeners, ≤two accepted client fds,
 *     the XCB file descriptor. No threads, no locks.
 *   - Pending-request state machines route asynchronous X11 selection
 *     traffic across the synchronous vsock host roundtrip.
 *   - Single instance per user session — installed as a systemd USER unit
 *     so X11/Wayland creds are inherited.
 *
 * Format coverage:
 *   CF_UNICODETEXT (13)    ↔ UTF8_STRING / text/plain;charset=utf-8
 *                            (iconv UTF-16LE ↔ UTF-8)
 *   CF_TEXT (1)            ↔ TEXT / STRING / text/plain
 *                            (iconv CP1252 ↔ UTF-8)
 *   CF_DIB (8) / CF_DIBV5  ↔ image/bmp  (BITMAPFILEHEADER munge)
 *   CF_HDROP (15)          ↔ text/uri-list  (FILE_DATA streamed bytes
 *                            handled in stage 2 of this file)
 *   registered "HTML Format" ↔ text/html  (CF_HTML envelope parse/build)
 *   any other registered    ↔ opaque pass-through under the same name
 *
 * Echo suppression is host-side (vm_clipboard.c writer_suppress/
 * reader_suppress flags) — this daemon has none of its own.
 *
 * Build: gcc -O2 -Wall appsandbox-clipboard.c \
 *        -lxcb -lxcb-xfixes -o appsandbox-clipboard
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <iconv.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/vm_sockets.h>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

/* ============================================================
 *   Wire protocol constants — must match vm_clipboard.c
 * ============================================================ */

#define CLIP_MAGIC                0x504C4341u   /* 'ACLP' on the wire */
#define CLIP_READY_MAGIC          0x59444C43u   /* 'CLDY' guest emits on accept */

#define CLIP_MSG_FORMAT_LIST      1u
#define CLIP_MSG_FORMAT_DATA_REQ  2u
#define CLIP_MSG_FORMAT_DATA_RESP 3u
#define CLIP_MSG_FILE_DATA        4u
#define CLIP_MSG_SYNC_ENABLE      12u

#define CLIP_MAX_FORMATS          64u
#define CLIP_MAX_PAYLOAD          (64u * 1024u * 1024u)
#define CLIP_FILE_CHUNK           (1u * 1024u * 1024u)

#define VSOCK_PORT_WRITER         5u
#define VSOCK_PORT_READER         6u

#pragma pack(push, 1)
struct ClipHeader {
    uint32_t magic;
    uint32_t msg_type;
    uint32_t format;
    uint32_t data_size;
};
struct ClipFileInfo {
    uint32_t path_len;
    uint64_t file_size;
    uint8_t  is_directory;
};
#pragma pack(pop)

/* Windows CF_* IDs we actually translate. */
#define CF_TEXT          1u
#define CF_BITMAP        2u   /* unsupported (handle) */
#define CF_DIB           8u
#define CF_UNICODETEXT  13u
#define CF_HDROP        15u
#define CF_DIBV5        17u

/* Threshold above which we send an X selection reply via the INCR
 * incremental-transfer protocol instead of a single property write.
 * The X server's max-request-size limits us to ~256KB chunks; staying
 * conservative avoids edge cases on smaller-max servers. */
#define X_INCR_THRESHOLD    (64u * 1024u)
#define X_INCR_CHUNK        (32u * 1024u)

/* ============================================================
 *   Logging
 * ============================================================ */

static FILE *g_log = NULL;

static void clip_log(const char *fmt, ...)
{
    va_list ap;
    char tbuf[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);

    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "[%s] ", tbuf);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

static void log_open(void)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.cache/appsandbox-clipboard.log", home);
    g_log = fopen(path, "a");
}

/* ============================================================
 *   Format conversion
 *
 * Each kind has paired (cf→x) and (x→cf) byte conversions. Sizes are
 * the host-clipboard sizes — what we actually ship in FORMAT_DATA_RESP
 * or write to an X selection property.
 *
 * Memory: all conversion functions return malloc()ed buffers; caller
 * frees. Returning NULL with *out_len=0 means "not convertible /
 * malformed input"; the daemon ships an empty payload in that case.
 * ============================================================ */

/* UTF-16LE (Windows wide) → UTF-8 via iconv. Strips trailing NUL pair
 * if present (Windows clipboard convention includes the terminator). */
static uint8_t *utf16le_to_utf8(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    if (in_len < 2) return NULL;

    /* Strip the trailing UTF-16 NUL. */
    if (in_len >= 2 && in[in_len - 1] == 0 && in[in_len - 2] == 0)
        in_len -= 2;

    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1) return NULL;

    size_t cap = in_len * 2 + 16;
    uint8_t *out = malloc(cap);
    if (!out) { iconv_close(cd); return NULL; }

    char *in_p  = (char *)in;
    size_t in_left = in_len;
    char *out_p = (char *)out;
    size_t out_left = cap;

    while (in_left > 0) {
        size_t r = iconv(cd, &in_p, &in_left, &out_p, &out_left);
        if (r == (size_t)-1 && errno == E2BIG) {
            size_t used = (size_t)(out_p - (char *)out);
            cap *= 2;
            uint8_t *new_out = realloc(out, cap);
            if (!new_out) { free(out); iconv_close(cd); return NULL; }
            out = new_out;
            out_p = (char *)out + used;
            out_left = cap - used;
            continue;
        }
        if (r == (size_t)-1) {
            free(out);
            iconv_close(cd);
            return NULL;
        }
    }
    iconv_close(cd);
    *out_len = (size_t)(out_p - (char *)out);
    return out;
}

/* UTF-8 → UTF-16LE, appending the trailing UTF-16 NUL terminator the
 * Windows clipboard convention expects. */
static uint8_t *utf8_to_utf16le(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    iconv_t cd = iconv_open("UTF-16LE", "UTF-8");
    if (cd == (iconv_t)-1) return NULL;

    /* Strip trailing NUL on input — some apps include one. */
    while (in_len > 0 && in[in_len - 1] == 0) in_len--;

    size_t cap = in_len * 4 + 16;
    uint8_t *out = malloc(cap);
    if (!out) { iconv_close(cd); return NULL; }

    char *in_p  = (char *)in;
    size_t in_left = in_len;
    char *out_p = (char *)out;
    size_t out_left = cap;

    while (in_left > 0) {
        size_t r = iconv(cd, &in_p, &in_left, &out_p, &out_left);
        if (r == (size_t)-1 && errno == E2BIG) {
            size_t used = (size_t)(out_p - (char *)out);
            cap *= 2;
            uint8_t *new_out = realloc(out, cap);
            if (!new_out) { free(out); iconv_close(cd); return NULL; }
            out = new_out;
            out_p = (char *)out + used;
            out_left = cap - used;
            continue;
        }
        if (r == (size_t)-1) {
            free(out);
            iconv_close(cd);
            return NULL;
        }
    }
    iconv_close(cd);

    /* Append UTF-16 NUL. */
    if (out_left < 2) {
        size_t used = (size_t)(out_p - (char *)out);
        uint8_t *new_out = realloc(out, used + 2);
        if (!new_out) { free(out); return NULL; }
        out = new_out;
        out_p = (char *)out + used;
    }
    *out_p++ = 0;
    *out_p++ = 0;
    *out_len = (size_t)(out_p - (char *)out);
    return out;
}

/* CP1252 (legacy Windows ANSI) → UTF-8 via iconv. */
static uint8_t *cp1252_to_utf8(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    iconv_t cd = iconv_open("UTF-8", "CP1252");
    if (cd == (iconv_t)-1) return NULL;
    while (in_len > 0 && in[in_len - 1] == 0) in_len--;

    size_t cap = in_len * 3 + 8;
    uint8_t *out = malloc(cap);
    if (!out) { iconv_close(cd); return NULL; }
    char *in_p = (char *)in;
    size_t in_left = in_len;
    char *out_p = (char *)out;
    size_t out_left = cap;
    while (in_left > 0) {
        if (iconv(cd, &in_p, &in_left, &out_p, &out_left) == (size_t)-1) {
            if (errno == E2BIG) {
                size_t used = (size_t)(out_p - (char *)out);
                cap *= 2;
                uint8_t *new_out = realloc(out, cap);
                if (!new_out) { free(out); iconv_close(cd); return NULL; }
                out = new_out;
                out_p = (char *)out + used;
                out_left = cap - used;
                continue;
            }
            free(out); iconv_close(cd); return NULL;
        }
    }
    iconv_close(cd);
    *out_len = (size_t)(out_p - (char *)out);
    return out;
}

static uint8_t *utf8_to_cp1252(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    /* //TRANSLIT lets iconv approximate Unicode chars not in CP1252
     * instead of erroring — what GTK does for legacy STRING fallbacks. */
    iconv_t cd = iconv_open("CP1252//TRANSLIT", "UTF-8");
    if (cd == (iconv_t)-1) return NULL;
    while (in_len > 0 && in[in_len - 1] == 0) in_len--;

    size_t cap = in_len + 16;
    uint8_t *out = malloc(cap);
    if (!out) { iconv_close(cd); return NULL; }
    char *in_p = (char *)in;
    size_t in_left = in_len;
    char *out_p = (char *)out;
    size_t out_left = cap - 1;   /* reserve NUL */
    while (in_left > 0) {
        if (iconv(cd, &in_p, &in_left, &out_p, &out_left) == (size_t)-1) {
            if (errno == E2BIG) {
                size_t used = (size_t)(out_p - (char *)out);
                cap *= 2;
                uint8_t *new_out = realloc(out, cap);
                if (!new_out) { free(out); iconv_close(cd); return NULL; }
                out = new_out;
                out_p = (char *)out + used;
                out_left = cap - 1 - used;
                continue;
            }
            free(out); iconv_close(cd); return NULL;
        }
    }
    iconv_close(cd);
    *out_p++ = 0;
    *out_len = (size_t)(out_p - (char *)out);
    return out;
}

/* CF_DIB ↔ image/bmp: a CF_DIB is exactly the body of a .bmp file
 * minus the 14-byte BITMAPFILEHEADER. Synthesizing/stripping the
 * file header is the conversion. No pixel work. */
static uint8_t *cf_dib_to_bmp(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    if (in_len < 16) return NULL;

    /* Pixel data offset = 14 (file header) + size of info header +
     * size of color table (if any). Color-table size: for ≤8bpp, derived
     * from biClrUsed; for ≥16bpp, usually 0 unless BI_BITFIELDS. */
    uint32_t info_size = (uint32_t)in[0] | ((uint32_t)in[1]<<8)
                       | ((uint32_t)in[2]<<16) | ((uint32_t)in[3]<<24);
    if (info_size < 12 || info_size > in_len) return NULL;

    uint16_t bpp = (info_size >= 16)
        ? (uint16_t)((uint16_t)in[14] | ((uint16_t)in[15]<<8))
        : 0;
    uint32_t clr_used = (info_size >= 36)
        ? ((uint32_t)in[32] | ((uint32_t)in[33]<<8)
           | ((uint32_t)in[34]<<16) | ((uint32_t)in[35]<<24))
        : 0;
    uint32_t compression = (info_size >= 20)
        ? ((uint32_t)in[16] | ((uint32_t)in[17]<<8)
           | ((uint32_t)in[18]<<16) | ((uint32_t)in[19]<<24))
        : 0;

    uint32_t palette_bytes = 0;
    if (bpp > 0 && bpp <= 8) {
        if (clr_used == 0) clr_used = 1u << bpp;
        palette_bytes = clr_used * 4;
    }
    /* BI_BITFIELDS adds three RGB masks (4 bytes each) after the info
     * header, before pixel data. */
    if (compression == 3 /* BI_BITFIELDS */) palette_bytes += 12;

    uint32_t pixel_offset = 14 + info_size + palette_bytes;

    size_t cap = 14 + in_len;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    out[0] = 'B'; out[1] = 'M';
    uint32_t total = (uint32_t)(14 + in_len);
    out[2] = total & 0xff; out[3] = (total>>8) & 0xff;
    out[4] = (total>>16) & 0xff; out[5] = (total>>24) & 0xff;
    out[6] = out[7] = out[8] = out[9] = 0;
    out[10] = pixel_offset & 0xff;
    out[11] = (pixel_offset>>8) & 0xff;
    out[12] = (pixel_offset>>16) & 0xff;
    out[13] = (pixel_offset>>24) & 0xff;
    memcpy(out + 14, in, in_len);
    *out_len = cap;
    return out;
}

static uint8_t *bmp_to_cf_dib(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    if (in_len < 14 || in[0] != 'B' || in[1] != 'M') return NULL;
    size_t body = in_len - 14;
    uint8_t *out = malloc(body);
    if (!out) return NULL;
    memcpy(out, in + 14, body);
    *out_len = body;
    return out;
}

/* CF_HTML envelope. Windows uses a versioned text header to delimit
 * the actual HTML fragment inside a larger document. Linux's
 * text/html is just raw HTML, so we have to add or strip the envelope
 * depending on direction.
 *
 * Envelope shape:
 *   Version:0.9\r\n
 *   StartHTML:00000xxx\r\n
 *   EndHTML:00000xxx\r\n
 *   StartFragment:00000xxx\r\n
 *   EndFragment:00000xxx\r\n
 *   <html><body>
 *   <!--StartFragment-->
 *   ...actual HTML fragment...
 *   <!--EndFragment-->
 *   </body></html>
 *
 * The five offset values are zero-padded to 10 digits (recursive — the
 * offsets count themselves). We pick a fixed header length and
 * back-fill so offsets are deterministic. */

static const char *kCfHtmlPrefix =
    "Version:0.9\r\n"
    "StartHTML:0000000105\r\n"
    "EndHTML:0000000000\r\n"
    "StartFragment:0000000141\r\n"
    "EndFragment:0000000000\r\n"
    "<html><body>\r\n"
    "<!--StartFragment-->";
static const char *kCfHtmlSuffix =
    "<!--EndFragment-->\r\n"
    "</body></html>";

static uint8_t *html_to_cf_html(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    size_t prefix_len = strlen(kCfHtmlPrefix);
    size_t suffix_len = strlen(kCfHtmlSuffix);
    size_t total = prefix_len + in_len + suffix_len + 1;
    uint8_t *out = malloc(total);
    if (!out) return NULL;
    memcpy(out, kCfHtmlPrefix, prefix_len);
    memcpy(out + prefix_len, in, in_len);
    memcpy(out + prefix_len + in_len, kCfHtmlSuffix, suffix_len);
    out[total - 1] = 0;

    /* Patch EndHTML and EndFragment offsets. The format is fixed-width
     * 10-digit decimal; find by ASCII scan. */
    char num[11];
    snprintf(num, sizeof(num), "%010zu", prefix_len + in_len + suffix_len);
    char *end_html = strstr((char *)out, "EndHTML:");
    if (end_html) memcpy(end_html + 8, num, 10);
    snprintf(num, sizeof(num), "%010zu", prefix_len + in_len);
    char *end_fragment = strstr((char *)out, "EndFragment:");
    if (end_fragment) memcpy(end_fragment + 12, num, 10);

    *out_len = total - 1;
    return out;
}

static uint8_t *cf_html_to_html(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    /* Find StartFragment / EndFragment offsets in the header. */
    if (in_len < 64) return NULL;
    const char *txt = (const char *)in;
    const char *sf = memmem(txt, in_len, "StartFragment:", 14);
    const char *ef = memmem(txt, in_len, "EndFragment:", 12);
    if (!sf || !ef) return NULL;
    long start = strtol(sf + 14, NULL, 10);
    long end   = strtol(ef + 12, NULL, 10);
    if (start <= 0 || end <= start || (size_t)end > in_len) return NULL;
    size_t body = (size_t)(end - start);
    uint8_t *out = malloc(body + 1);
    if (!out) return NULL;
    memcpy(out, in + start, body);
    out[body] = 0;
    *out_len = body;
    return out;
}

/* CF_HDROP ↔ text/uri-list (small URI body — actual file bytes flow
 * separately via the FILE_DATA streaming protocol, handled lower down).
 *
 * CF_HDROP wire layout:
 *   DROPFILES { uint32 pFiles; uint16 pt[2]; uint8 fNC; uint8 fWide; }
 *   then double-NUL-terminated list of wide-char (UTF-16LE) absolute paths.
 */
struct dropfiles_hdr {
    uint32_t pFiles;
    uint32_t pt_xy;       /* POINT */
    uint32_t fNC_fWide;   /* fNC (low byte), fWide (high byte) */
};

static uint8_t *cf_hdrop_to_urilist(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    if (in_len < sizeof(struct dropfiles_hdr) + 2) return NULL;
    struct dropfiles_hdr h;
    memcpy(&h, in, sizeof(h));
    if (h.pFiles >= in_len) return NULL;

    /* Linux daemon sends URIs pointing at the temp dir the host will
     * populate via FILE_DATA. Path lookup happens after the FILE_DATA
     * stream completes; here we just enumerate filenames. */
    bool wide = (h.fNC_fWide >> 24) & 1;
    const uint8_t *p   = in + h.pFiles;
    const uint8_t *end = in + in_len;

    size_t cap = 256;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    size_t used = 0;

    while (p < end) {
        const uint8_t *name = p;
        size_t bytes;
        if (wide) {
            const uint8_t *q = p;
            while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q += 2;
            if (q + 1 >= end) break;
            bytes = (size_t)(q - p);
            if (bytes == 0) break;   /* double-NUL terminator */

            size_t u8_len = 0;
            uint8_t *u8 = utf16le_to_utf8(name, bytes, &u8_len);
            if (!u8) { p = q + 2; continue; }
            size_t entry_len = 7 + u8_len + 2;   /* "file://" + path + "\r\n" */
            if (used + entry_len > cap) {
                while (used + entry_len > cap) cap *= 2;
                uint8_t *no = realloc(out, cap);
                if (!no) { free(u8); free(out); return NULL; }
                out = no;
            }
            memcpy(out + used, "file://", 7); used += 7;
            memcpy(out + used, u8, u8_len);    used += u8_len;
            out[used++] = '\r'; out[used++] = '\n';
            free(u8);
            p = q + 2;
        } else {
            const uint8_t *q = p;
            while (q < end && *q) q++;
            if (q >= end) break;
            bytes = (size_t)(q - p);
            if (bytes == 0) break;
            size_t entry_len = 7 + bytes + 2;
            if (used + entry_len > cap) {
                while (used + entry_len > cap) cap *= 2;
                uint8_t *no = realloc(out, cap);
                if (!no) { free(out); return NULL; }
                out = no;
            }
            memcpy(out + used, "file://", 7); used += 7;
            memcpy(out + used, name, bytes);  used += bytes;
            out[used++] = '\r'; out[used++] = '\n';
            p = q + 1;
        }
    }
    *out_len = used;
    return out;
}

/* ============================================================
 *   Format mapping table
 *
 * One Windows CF_* may correspond to several X11 atoms (e.g. text has
 * UTF8_STRING, STRING, TEXT, text/plain, text/plain;charset=utf-8).
 * Same CF_* points at the same conversion; multiple X atoms point at it.
 * ============================================================ */

enum conv_kind {
    CONV_PASSTHROUGH,    /* identity, used for unknown registered formats */
    CONV_UTF16LE,        /* CF_UNICODETEXT ↔ UTF-8 */
    CONV_CP1252,         /* CF_TEXT        ↔ UTF-8 */
    CONV_DIB_BMP,        /* CF_DIB         ↔ image/bmp */
    CONV_HTML,           /* CF_HTML        ↔ text/html */
    CONV_HDROP,          /* CF_HDROP       ↔ text/uri-list (URIs only) */
};

struct fmt_entry {
    uint32_t cf;             /* Windows CF_* (or synthetic id for registered) */
    const char *xname;       /* X11 atom name */
    enum conv_kind conv;
};

/* Order matters: when we have multiple X atoms for one CF_*, the first
 * is the preferred target we advertise in TARGETS and prefer to fetch. */
static const struct fmt_entry kFormats[] = {
    /* Text — Unicode preferred. */
    { CF_UNICODETEXT, "UTF8_STRING",               CONV_UTF16LE },
    { CF_UNICODETEXT, "text/plain;charset=utf-8",  CONV_UTF16LE },
    { CF_UNICODETEXT, "text/plain",                CONV_UTF16LE },

    /* Text — legacy. */
    { CF_TEXT,        "TEXT",   CONV_CP1252 },
    { CF_TEXT,        "STRING", CONV_CP1252 },

    /* Image. */
    { CF_DIB,         "image/bmp", CONV_DIB_BMP },
    { CF_DIBV5,       "image/bmp", CONV_DIB_BMP },

    /* Files. x-special/gnome-copied-files first because Nautilus uses
     * its presence (not text/uri-list) to enable the Paste menu item
     * on the desktop / file manager view. */
    { CF_HDROP,       "x-special/gnome-copied-files", CONV_HDROP },
    { CF_HDROP,       "text/uri-list",                CONV_HDROP },
};
#define N_FORMATS (sizeof(kFormats) / sizeof(kFormats[0]))

/* Registered-format CF_HTML maps to text/html. Resolved at startup
 * since the CF_HTML id is assigned dynamically. */
static uint32_t g_cf_html_id = 0;        /* host sends this in the format id */

/* X atom resolution cache, populated at startup. */
static xcb_atom_t g_x_atom[N_FORMATS];
static xcb_atom_t g_x_atom_html = XCB_ATOM_NONE;
static struct {
    uint32_t cf;
    xcb_atom_t target;
} g_guest_formats[CLIP_MAX_FORMATS];
static uint32_t g_guest_format_count;

static enum conv_kind cf_to_conv(uint32_t cf)
{
    if (cf == g_cf_html_id) return CONV_HTML;
    for (size_t i = 0; i < N_FORMATS; i++)
        if (kFormats[i].cf == cf) return kFormats[i].conv;
    return CONV_PASSTHROUGH;
}

/* Convert a payload from Windows CF_* bytes to bytes for an X11 target. */
static uint8_t *convert_cf_to_x(uint32_t cf, const uint8_t *in, size_t in_len,
                                size_t *out_len)
{
    switch (cf_to_conv(cf)) {
    case CONV_UTF16LE: return utf16le_to_utf8(in, in_len, out_len);
    case CONV_CP1252:  return cp1252_to_utf8(in, in_len, out_len);
    case CONV_DIB_BMP: return cf_dib_to_bmp(in, in_len, out_len);
    case CONV_HTML:    return cf_html_to_html(in, in_len, out_len);
    case CONV_HDROP:   return cf_hdrop_to_urilist(in, in_len, out_len);
    case CONV_PASSTHROUGH:
    default: {
        uint8_t *out = malloc(in_len ? in_len : 1);
        if (!out) { *out_len = 0; return NULL; }
        memcpy(out, in, in_len);
        *out_len = in_len;
        return out;
    }
    }
}

static uint8_t *convert_x_to_cf(uint32_t cf, const uint8_t *in, size_t in_len,
                                size_t *out_len)
{
    switch (cf_to_conv(cf)) {
    case CONV_UTF16LE: return utf8_to_utf16le(in, in_len, out_len);
    case CONV_CP1252:  return utf8_to_cp1252(in, in_len, out_len);
    case CONV_DIB_BMP: return bmp_to_cf_dib(in, in_len, out_len);
    case CONV_HTML:    return html_to_cf_html(in, in_len, out_len);
    case CONV_HDROP:
        /* HDROP synthesis from a uri-list requires resolving each URI
         * into a host-side temp file path; that happens after the
         * FILE_DATA stream completes. For now the reader-side just
         * ships the URI list opaquely. */
    case CONV_PASSTHROUGH:
    default: {
        uint8_t *out = malloc(in_len ? in_len : 1);
        if (!out) { *out_len = 0; return NULL; }
        memcpy(out, in, in_len);
        *out_len = in_len;
        return out;
    }
    }
}

/* ============================================================
 *   X11 / XCB setup
 * ============================================================ */

static xcb_connection_t *g_xcb = NULL;
static xcb_screen_t     *g_screen = NULL;
static xcb_window_t      g_window = 0;
static uint8_t           g_xfixes_event_base = 0;

/* Standard atoms we always need. */
static xcb_atom_t a_CLIPBOARD;
static xcb_atom_t a_TARGETS;
static xcb_atom_t a_INCR;
static xcb_atom_t a_MULTIPLE;
static xcb_atom_t a_TIMESTAMP;
static xcb_atom_t a_ATOM;
static xcb_atom_t a_CLIP_DATA;

static xcb_atom_t intern_atom(const char *name)
{
    xcb_intern_atom_cookie_t c =
        xcb_intern_atom(g_xcb, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(g_xcb, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static int xcb_setup(void)
{
    int screen_num;
    g_xcb = xcb_connect(NULL, &screen_num);
    if (!g_xcb || xcb_connection_has_error(g_xcb)) {
        clip_log("xcb_connect: failed (DISPLAY=%s)", getenv("DISPLAY"));
        return -1;
    }

    const xcb_setup_t *setup = xcb_get_setup(g_xcb);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) xcb_screen_next(&it);
    g_screen = it.data;

    g_window = xcb_generate_id(g_xcb);
    xcb_create_window(g_xcb, XCB_COPY_FROM_PARENT, g_window, g_screen->root,
                      0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, g_screen->root_visual,
                      0, NULL);

    /* XFIXES extension for selection-owner notifications. */
    const xcb_query_extension_reply_t *xf =
        xcb_get_extension_data(g_xcb, &xcb_xfixes_id);
    if (!xf || !xf->present) {
        clip_log("XFIXES extension not present");
        return -1;
    }
    g_xfixes_event_base = xf->first_event;
    xcb_xfixes_query_version_cookie_t qc =
        xcb_xfixes_query_version(g_xcb, 5, 0);
    xcb_xfixes_query_version_reply_t *qr =
        xcb_xfixes_query_version_reply(g_xcb, qc, NULL);
    free(qr);

    /* Static atoms. */
    a_CLIPBOARD = intern_atom("CLIPBOARD");
    a_TARGETS   = intern_atom("TARGETS");
    a_INCR      = intern_atom("INCR");
    a_MULTIPLE  = intern_atom("MULTIPLE");
    a_TIMESTAMP = intern_atom("TIMESTAMP");
    a_ATOM      = XCB_ATOM_ATOM;
    a_CLIP_DATA = intern_atom("_APPSANDBOX_CLIP_DATA");

    /* Per-format atoms. */
    for (size_t i = 0; i < N_FORMATS; i++)
        g_x_atom[i] = intern_atom(kFormats[i].xname);
    g_x_atom_html = intern_atom("text/html");

    /* Watch CLIPBOARD owner changes. We need all three event flavors —
     * Mutter selects the same combination on its own selection window
     * (meta-x11-selection.c:506) and we'd otherwise miss the cases
     * where an X client owner just closes its window. */
    xcb_xfixes_select_selection_input(g_xcb, g_window, a_CLIPBOARD,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);

    /* Property events on our own window — we use property writes as
     * the channel for incoming selection data. */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(g_xcb, g_window, XCB_CW_EVENT_MASK, &mask);

    xcb_flush(g_xcb);
    clip_log("XCB connected (DISPLAY=%s, window=0x%x)",
             getenv("DISPLAY") ? getenv("DISPLAY") : "<unset>",
             g_window);
    return 0;
}

/* Given an X target atom (UTF8_STRING etc), return the matching CF_*
 * we'd ship over the wire. Returns 0 if not supported. */
static uint32_t target_to_cf(xcb_atom_t target)
{
    if (target == g_x_atom_html && g_cf_html_id) return g_cf_html_id;
    for (size_t i = 0; i < N_FORMATS; i++)
        if (g_x_atom[i] == target) return kFormats[i].cf;
    return 0;
}

static xcb_atom_t cf_to_offered_target(uint32_t cf, const xcb_atom_t *atoms,
                                      uint32_t count)
{
    if (cf == g_cf_html_id && g_x_atom_html) return g_x_atom_html;
    for (size_t i = 0; i < N_FORMATS; i++) {
        if (kFormats[i].cf != cf) continue;
        for (uint32_t j = 0; j < count; j++)
            if (atoms[j] == g_x_atom[i]) return g_x_atom[i];
    }
    return XCB_ATOM_NONE;
}

static xcb_atom_t guest_target_for_cf(uint32_t cf)
{
    for (uint32_t i = 0; i < g_guest_format_count; i++)
        if (g_guest_formats[i].cf == cf) return g_guest_formats[i].target;
    return XCB_ATOM_NONE;
}

/* ============================================================
 *   Vsock listeners + framed I/O
 * ============================================================ */

static int vsock_listen(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { clip_log("vsock socket: %s", strerror(errno)); return -1; }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        clip_log("vsock bind :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        clip_log("vsock listen :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    return s;
}

static int recv_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int send_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int send_clip_msg(int fd, uint32_t msg_type, uint32_t format,
                          const uint8_t *body, uint32_t body_len)
{
    struct ClipHeader h = {
        .magic     = CLIP_MAGIC,
        .msg_type  = msg_type,
        .format    = format,
        .data_size = body_len,
    };
    if (send_exact(fd, &h, sizeof(h)) < 0) return -1;
    if (body_len > 0 && send_exact(fd, body, body_len) < 0) return -1;
    return 0;
}

/* ============================================================
 *   State machine
 *
 * Two vsock connections (writer, reader), one X connection. State
 * transitions are driven by epoll wakeups in a single thread.
 * ============================================================ */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Sync gate (host-controlled via SYNC_ENABLE on writer channel). */
static bool g_sync_enabled = false;

/* Latest set of formats the host has advertised that we should own
 * the CLIPBOARD for. Built from FORMAT_LIST messages. */
static uint32_t g_host_formats[CLIP_MAX_FORMATS];
static int      g_host_format_count = 0;

/* Forward declarations needed by the VM→Windows file paste helpers
 * defined below — the actual globals are defined further down where
 * the rest of the per-session state lives. */
static int g_writer_fd;
static int g_reader_fd;

/* ============================================================
 *   VM → Windows file paste
 *
 * When the host issues FORMAT_DATA_REQ(CF_HDROP), we issued
 * XConvertSelection for x-special/gnome-copied-files (preferred) or
 * text/uri-list. The X reply body is a list of file:// URIs. We
 * parse them, then for each path stream FILE_DATA messages to the
 * host (recursing into directories), and finally an empty
 * FORMAT_DATA_RESP(CF_HDROP) to mark end-of-stream — exactly the
 * sequence tools/agent/appsandbox-clipboard-reader.c emits.
 *
 * Wire format per FILE_DATA, copied verbatim from the Windows path:
 *   ClipHeader{ msg=4, fmt=0, data_size=sizeof(ClipFileInfo)+wpath_bytes }
 *   ClipFileInfo{ path_len, file_size, is_directory }
 *   wpath_bytes of UTF-16LE relative path (NO terminator,
 *                                          backslash separators)
 *   file_size bytes of raw file body  (NOT counted in data_size)
 * ============================================================ */

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place percent-decode (file:// paths from Nautilus are
 * %20-encoded for spaces etc.). */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = hex_val((unsigned char)r[1]);
            int lo = hex_val((unsigned char)r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Parse an x-special/gnome-copied-files or text/uri-list body, extract
 * the local filesystem paths from file:// URIs. Returns count;
 * *out_paths is a malloc'd array of malloc'd UTF-8 path strings the
 * caller must free. */
static int parse_uri_list(const uint8_t *body, size_t body_len,
                          char ***out_paths)
{
    char **paths = NULL;
    int count = 0, cap = 0;
    const uint8_t *p = body, *end = body + body_len;

    *out_paths = NULL;

    while (p < end) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        const uint8_t *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            line_end++;
        size_t line_len = (size_t)(line_end - p);

        if (line_len >= 7 && memcmp(p, "file://", 7) == 0) {
            /* Strip authority if present: file://host/path → /path */
            const uint8_t *path_start = p + 7;
            size_t path_len = line_len - 7;
            if (path_len > 0 && *path_start != '/') {
                /* Has an authority component — find next '/' */
                const uint8_t *slash = memchr(path_start, '/', path_len);
                if (slash) {
                    path_len -= (size_t)(slash - path_start);
                    path_start = slash;
                } else {
                    path_len = 0;
                }
            }
            if (path_len > 0) {
                char *path = malloc(path_len + 1);
                if (path) {
                    memcpy(path, path_start, path_len);
                    path[path_len] = '\0';
                    url_decode(path);

                    if (count == cap) {
                        cap = cap ? cap * 2 : 8;
                        char **np = realloc(paths,
                                            (size_t)cap * sizeof(char *));
                        if (!np) { free(path); break; }
                        paths = np;
                    }
                    paths[count++] = path;
                }
            }
        }
        /* "copy" / "cut" header line and any text/x-moz-url etc. are
         * silently skipped — only file:// is relevant. */
        p = line_end;
    }

    *out_paths = paths;
    return count;
}

/* Mirror of tools/agent/appsandbox-clipboard-reader.c:clip_send_file_entry.
 * Sends a single file (with body) or a directory (recursively) on the
 * reader channel. Returns 0 on success, -1 only on socket failure
 * (caller bails on that — the host side will close the stream and
 * vm_clipboard.c's reader_thread loops back to reconnect). Returns 0
 * on per-file errors so subsequent siblings still ship. */
static int send_file_entry_linux(int fd, const char *full_path,
                                 const char *rel_path)
{
    struct stat st;
    if (lstat(full_path, &st) < 0) return 0;
    if (S_ISLNK(st.st_mode))       return 0;          /* never follow */
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) return 0;

    bool is_dir = S_ISDIR(st.st_mode);

    /* Windows side splits paths on backslash (clip_ensure_parent_dir,
     * vm_clipboard.c:258). Translate POSIX '/' before UTF-16-encoding. */
    size_t rel_len = strlen(rel_path);
    char wsep[1024];
    if (rel_len + 1 > sizeof(wsep)) return 0;
    for (size_t i = 0; i <= rel_len; i++)
        wsep[i] = (rel_path[i] == '/') ? '\\' : rel_path[i];

    size_t wpath_len = 0;
    uint8_t *wpath = utf8_to_utf16le((const uint8_t *)wsep, strlen(wsep),
                                     &wpath_len);
    if (!wpath) return 0;
    /* utf8_to_utf16le() appends a U+0000 — strip per wire spec. */
    if (wpath_len >= 2 && wpath[wpath_len-2] == 0 && wpath[wpath_len-1] == 0)
        wpath_len -= 2;

    struct ClipFileInfo fi;
    fi.path_len     = (uint32_t)wpath_len;
    fi.file_size    = is_dir ? 0 : (uint64_t)st.st_size;
    fi.is_directory = is_dir ? 1 : 0;

    struct ClipHeader hdr;
    hdr.magic     = CLIP_MAGIC;
    hdr.msg_type  = CLIP_MSG_FILE_DATA;
    hdr.format    = 0;
    hdr.data_size = (uint32_t)(sizeof(fi) + wpath_len);

    if (send_exact(fd, &hdr, sizeof(hdr)) < 0 ||
        send_exact(fd, &fi,  sizeof(fi))  < 0 ||
        (wpath_len > 0 && send_exact(fd, wpath, wpath_len) < 0)) {
        free(wpath);
        return -1;
    }
    free(wpath);

    if (is_dir) {
        DIR *d = opendir(full_path);
        if (!d) return 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child_full[2048], child_rel[2048];
            int n1 = snprintf(child_full, sizeof(child_full), "%s/%s",
                              full_path, de->d_name);
            int n2 = snprintf(child_rel,  sizeof(child_rel),  "%s/%s",
                              rel_path, de->d_name);
            if (n1 < 0 || n2 < 0 ||
                n1 >= (int)sizeof(child_full) ||
                n2 >= (int)sizeof(child_rel))
                continue;
            int r = send_file_entry_linux(fd, child_full, child_rel);
            if (r < 0) { closedir(d); return -1; }
        }
        closedir(d);
        return 0;
    }

    /* File body — host expects exactly file_size bytes. If a partial
     * read happens, pad to keep the stream in sync (host parses the
     * next ClipHeader right after). */
    FILE *fp = fopen(full_path, "rb");
    uint64_t remaining = fi.file_size;
    uint8_t buf[64 * 1024];
    while (remaining > 0) {
        size_t want = remaining > sizeof(buf) ? sizeof(buf)
                                              : (size_t)remaining;
        size_t got = fp ? fread(buf, 1, want, fp) : 0;
        if (got == 0) {
            memset(buf, 0, want);
            got = want;
        }
        if (send_exact(fd, buf, got) < 0) {
            if (fp) fclose(fp);
            return -1;
        }
        remaining -= got;
    }
    if (fp) fclose(fp);
    return 0;
}

/* Orchestrate VM→host CF_HDROP: parse URIs from the X selection body,
 * stream each as FILE_DATA(s), then send the empty FORMAT_DATA_RESP
 * end-marker. */
static void deliver_hdrop_to_host(const uint8_t *body, size_t body_len)
{
    if (g_reader_fd < 0) return;

    char **paths = NULL;
    int n_paths = parse_uri_list(body, body_len, &paths);
    clip_log("hdrop→host: %d URIs from selection", n_paths);

    for (int i = 0; i < n_paths; i++) {
        const char *full = paths[i];
        /* The Windows reference uses just the basename for the top-
         * level rel_path (appsandbox-clipboard-reader.c:362). Match
         * that — clip_build_hdrop_from_temp on the host enumerates
         * temp_dir's top-level entries only. */
        const char *base = strrchr(full, '/');
        const char *rel  = base ? base + 1 : full;
        /* Trailing-slash directory URIs: walk back one more level. */
        if (*rel == '\0' && base && base > full) {
            const char *prev = base - 1;
            while (prev > full && *prev != '/') prev--;
            rel = (*prev == '/') ? prev + 1 : prev;
        }
        if (*rel == '\0') continue;
        if (send_file_entry_linux(g_reader_fd, full, rel) < 0) {
            clip_log("hdrop→host: socket send failed mid-stream");
            break;
        }
    }
    for (int i = 0; i < n_paths; i++) free(paths[i]);
    free(paths);

    /* End marker — host's clip_reader_handle_message treats empty
     * FORMAT_DATA_RESP(CF_HDROP) as the is_hdrop flag for this slot
     * (vm_clipboard.c:869). */
    send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_DATA_RESP, CF_HDROP, NULL, 0);
}

/* CF_HDROP collection state. When an X11 requestor (typically Nautilus
 * via Mutter) asks for x-special/gnome-copied-files or text/uri-list,
 * we issue FORMAT_DATA_REQ(CF_HDROP) to the host. The host streams a
 * series of FILE_DATA messages (each is ClipHeader{data_size=
 * sizeof(ClipFileInfo)+path_bytes} + ClipFileInfo + path_bytes + then
 * file_size raw bytes), then sends an empty FORMAT_DATA_RESP as the
 * end-of-stream marker. We write each file under tmp_dir/ and
 * accumulate file:// URIs. On the empty RESP we build the per-target
 * body (text/uri-list or x-special/gnome-copied-files) and complete
 * the X SelectionRequest by writing the property + sending
 * SelectionNotify.
 *
 * tmp_dir is created in $XDG_RUNTIME_DIR (tmpfs, auto-cleaned at
 * logout) with a timestamp+pid suffix so consecutive pastes don't
 * fight over the same files. Old tmp dirs are not actively cleaned
 * mid-session — they go away when /run/user/$UID is recycled. */
struct hdrop_collect {
    bool   active;
    char   tmp_dir[512];
    char **uris;
    size_t n_uris;
    size_t cap_uris;
};
static struct hdrop_collect g_hdrop;

static int mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Reject any path that could write outside tmp_dir. */
static bool path_safe(const char *p)
{
    if (!p || !*p) return false;
    if (p[0] == '/') return false;                      /* no absolute */
    if (strstr(p, "..")) return false;                  /* no parent escape */
    if (strlen(p) > 400) return false;                  /* sanity cap */
    return true;
}

static void hdrop_collect_start(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/tmp";
    snprintf(g_hdrop.tmp_dir, sizeof(g_hdrop.tmp_dir),
             "%s/appsandbox-clip-%ld-%d",
             rt, (long)time(NULL), (int)getpid());
    if (mkdir(g_hdrop.tmp_dir, 0700) < 0 && errno != EEXIST) {
        clip_log("hdrop: mkdir(%s) failed: %s",
                 g_hdrop.tmp_dir, strerror(errno));
        g_hdrop.tmp_dir[0] = '\0';
        return;
    }
    g_hdrop.active = true;
    g_hdrop.uris = NULL;
    g_hdrop.n_uris = 0;
    g_hdrop.cap_uris = 0;
    clip_log("hdrop: collecting into %s", g_hdrop.tmp_dir);
}

static void hdrop_add_uri(const char *uri)
{
    if (g_hdrop.n_uris == g_hdrop.cap_uris) {
        size_t nc = g_hdrop.cap_uris ? g_hdrop.cap_uris * 2 : 16;
        char **nv = realloc(g_hdrop.uris, nc * sizeof(char *));
        if (!nv) return;
        g_hdrop.uris = nv;
        g_hdrop.cap_uris = nc;
    }
    g_hdrop.uris[g_hdrop.n_uris++] = strdup(uri);
}

static void hdrop_collect_clear(void)
{
    for (size_t i = 0; i < g_hdrop.n_uris; i++) free(g_hdrop.uris[i]);
    free(g_hdrop.uris);
    g_hdrop.uris = NULL;
    g_hdrop.n_uris = 0;
    g_hdrop.cap_uris = 0;
    g_hdrop.active = false;
    /* Leave tmp_dir on disk — Nautilus may still be copying from it. */
}

/* Pending X SelectionRequest waiting on a host roundtrip. We only
 * service one at a time — sufficient because vm_clipboard.c is also
 * one-in-flight. If a second request arrives while one is pending,
 * we reject it with a None property reply (X11 convention for "no").
 *
 * If the converted payload exceeds X_INCR_THRESHOLD we keep the bytes
 * in incr_buf and drive the X11 INCR protocol from the PropertyNotify
 * (state=PropertyDelete) events that the requestor sends us as it
 * consumes each chunk. We had to set PropertyChangeMask on the
 * requestor's window first — see deliver_host_data_to_x. */
struct pending_x_req {
    bool         active;
    xcb_window_t requestor;
    xcb_atom_t   property;
    xcb_atom_t   target;
    xcb_atom_t   selection;
    xcb_timestamp_t time;
    uint32_t     cf;          /* host format we requested */
    /* INCR state */
    bool         incr;
    uint8_t     *incr_buf;
    size_t       incr_len;
    size_t       incr_off;
    bool         incr_eof_sent;
};
static struct pending_x_req g_pending_x;

static void finish_pending_x(void)
{
    if (g_hdrop.active) hdrop_collect_clear();
    free(g_pending_x.incr_buf);
    memset(&g_pending_x, 0, sizeof(g_pending_x));
}

static void deliver_hdrop_to_x(void);

/* Pending host FORMAT_DATA_REQ waiting on an X SelectionNotify reader. */
struct pending_host_req {
    bool         active;
    uint32_t     cf;
    xcb_atom_t   target;
    xcb_atom_t   property;
    xcb_window_t owner;
    xcb_timestamp_t time;
    bool         incr;        /* receiving via INCR protocol */
    uint8_t     *accum;       /* growing buffer */
    size_t       accum_len;
    size_t       accum_cap;
};
static struct pending_host_req g_pending_host;

/* Accepted clients (one per channel at a time). */
static int g_writer_fd = -1;
static int g_reader_fd = -1;
static xcb_window_t g_selection_owner;
static xcb_timestamp_t g_selection_time;
static bool g_targets_dirty;

/* When we're owner of CLIPBOARD via a FORMAT_LIST from the host, hold
 * onto the last set of formats we advertised so SelectionRequest TARGETS
 * can respond without a host roundtrip. */
static bool g_we_own_clipboard = false;

/* Build the TARGETS atom list to advertise based on g_host_formats[]. */
static size_t build_targets_atom_list(xcb_atom_t *out, size_t cap)
{
    size_t n = 0;
    if (n < cap) out[n++] = a_TARGETS;
    if (n < cap) out[n++] = a_TIMESTAMP;

    for (int i = 0; i < g_host_format_count && n < cap; i++) {
        uint32_t cf = g_host_formats[i];
        if (cf == g_cf_html_id && g_x_atom_html) {
            out[n++] = g_x_atom_html;
            continue;
        }
        for (size_t j = 0; j < N_FORMATS && n < cap; j++) {
            if (kFormats[j].cf == cf) out[n++] = g_x_atom[j];
        }
    }
    return n;
}

/* Send a single SelectionNotify back to the requestor. */
static void send_selection_notify(xcb_window_t requestor, xcb_atom_t selection,
                                   xcb_atom_t target, xcb_atom_t property,
                                   xcb_timestamp_t time)
{
    xcb_selection_notify_event_t ev = {0};
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.time          = time;
    ev.requestor     = requestor;
    ev.selection     = selection;
    ev.target        = target;
    ev.property      = property;
    xcb_send_event(g_xcb, 0, requestor,
                   XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    xcb_flush(g_xcb);
}

/* ---- Host→guest path: serve SelectionRequest events ---- */

static void serve_targets(xcb_window_t requestor, xcb_atom_t selection,
                          xcb_atom_t property, xcb_timestamp_t time)
{
    xcb_atom_t atoms[N_FORMATS + 4];
    size_t n = build_targets_atom_list(atoms, N_FORMATS + 4);
    xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE, requestor, property,
                        a_ATOM, 32, (uint32_t)n, atoms);
    send_selection_notify(requestor, selection, a_TARGETS, property, time);
}

/* When a host FORMAT_DATA_RESP arrives, complete the pending X selection
 * request by writing the converted bytes to the requestor's property.
 *
 * For payloads >X_INCR_THRESHOLD we follow the X11 INCR protocol:
 *   1. Set property to type=INCR, format=32, value=byte count
 *   2. Send SelectionNotify
 *   3. Requestor reads INCR property and deletes it
 *   4. We see PropertyNotify (state=Delete) → write next chunk with
 *      PROP_MODE_REPLACE (the property is gone, so no type collision)
 *   5. Repeat until done; final write is zero-length to signal EOF
 *
 * Crucially: we must select PropertyChangeMask on the requestor's
 * window or we never see the delete events. Mutter does the same
 * (meta-x11-selection-output-stream.c:236). */
static void send_next_incr_chunk(void);

static void deliver_host_data_to_x(const uint8_t *cf_bytes, size_t cf_len)
{
    if (!g_pending_x.active) return;

    size_t x_len = 0;
    uint8_t *x_bytes = convert_cf_to_x(g_pending_x.cf, cf_bytes, cf_len, &x_len);
    if (!x_bytes) {
        clip_log("convert CF %u → X failed, returning None", g_pending_x.cf);
        send_selection_notify(g_pending_x.requestor, g_pending_x.selection,
                              g_pending_x.target, XCB_ATOM_NONE,
                              g_pending_x.time);
        finish_pending_x();
        return;
    }

    if (x_len <= X_INCR_THRESHOLD) {
        xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE,
                            g_pending_x.requestor, g_pending_x.property,
                            g_pending_x.target, 8,
                            (uint32_t)x_len, x_bytes);
        send_selection_notify(g_pending_x.requestor, g_pending_x.selection,
                              g_pending_x.target, g_pending_x.property,
                              g_pending_x.time);
        xcb_flush(g_xcb);
        clip_log("served CF %u → X target %u (%zu bytes)",
                 g_pending_x.cf, g_pending_x.target, x_len);
        free(x_bytes);
        finish_pending_x();
        return;
    }

    /* INCR path. Buffer the data, prime PropertyChangeMask on the
     * requestor (preserving any other events they had selected), write
     * the INCR header, then wait for PropertyDelete to pump chunks. */
    g_pending_x.incr = true;
    g_pending_x.incr_buf = x_bytes;
    g_pending_x.incr_len = x_len;
    g_pending_x.incr_off = 0;
    g_pending_x.incr_eof_sent = false;

    xcb_get_window_attributes_cookie_t ac =
        xcb_get_window_attributes(g_xcb, g_pending_x.requestor);
    xcb_get_window_attributes_reply_t *ar =
        xcb_get_window_attributes_reply(g_xcb, ac, NULL);
    uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (ar) {
        event_mask |= ar->your_event_mask;
        free(ar);
    }
    xcb_change_window_attributes(g_xcb, g_pending_x.requestor,
                                 XCB_CW_EVENT_MASK, &event_mask);

    uint32_t total = (uint32_t)x_len;
    xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE,
                        g_pending_x.requestor, g_pending_x.property,
                        a_INCR, 32, 1, &total);
    send_selection_notify(g_pending_x.requestor, g_pending_x.selection,
                          g_pending_x.target, g_pending_x.property,
                          g_pending_x.time);
    xcb_flush(g_xcb);
    clip_log("INCR start CF %u → X target %u, total %zu bytes",
             g_pending_x.cf, g_pending_x.target, x_len);
}

/* Called from PropertyNotify(state=Delete) on the requestor window. */
static void send_next_incr_chunk(void)
{
    if (!g_pending_x.active || !g_pending_x.incr) return;

    if (g_pending_x.incr_eof_sent) {
        /* Final empty chunk was already written and now consumed —
         * transfer complete. */
        clip_log("INCR done CF %u (%zu bytes)",
                 g_pending_x.cf, g_pending_x.incr_len);
        finish_pending_x();
        return;
    }

    size_t remaining = g_pending_x.incr_len - g_pending_x.incr_off;
    size_t chunk = remaining > X_INCR_CHUNK ? X_INCR_CHUNK : remaining;

    xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE,
                        g_pending_x.requestor, g_pending_x.property,
                        g_pending_x.target, 8,
                        (uint32_t)chunk,
                        g_pending_x.incr_buf + g_pending_x.incr_off);
    xcb_flush(g_xcb);
    g_pending_x.incr_off += chunk;

    if (chunk == 0) {
        /* That was the EOF marker — wait for one more delete then we're
         * done. */
        g_pending_x.incr_eof_sent = true;
    } else if (g_pending_x.incr_off == g_pending_x.incr_len) {
        /* Need a zero-length chunk to signal EOF; the requestor will
         * delete this real chunk first, then we send the EOF on the
         * next delete. Setting the size to incr_len == incr_off makes
         * the next call's chunk == 0. */
    }
}

/* Complete a pending CF_HDROP SelectionRequest. Builds the per-target
 * body (text/uri-list or x-special/gnome-copied-files) from the URIs
 * we accumulated during the FILE_DATA stream, writes it to the
 * requestor's property, and sends SelectionNotify. */
static void deliver_hdrop_to_x(void)
{
    if (!g_pending_x.active) return;

    /* Decide output flavor by target atom name. */
    bool gnome_format = false;
    xcb_get_atom_name_cookie_t nc =
        xcb_get_atom_name(g_xcb, g_pending_x.target);
    xcb_get_atom_name_reply_t *nr = xcb_get_atom_name_reply(g_xcb, nc, NULL);
    if (nr) {
        int len = xcb_get_atom_name_name_length(nr);
        const char *name = xcb_get_atom_name_name(nr);
        if (len == (int)strlen("x-special/gnome-copied-files") &&
            memcmp(name, "x-special/gnome-copied-files", len) == 0)
            gnome_format = true;
        free(nr);
    }

    if (g_hdrop.n_uris == 0) {
        clip_log("hdrop: no URIs collected — returning None");
        send_selection_notify(g_pending_x.requestor, g_pending_x.selection,
                              g_pending_x.target, XCB_ATOM_NONE,
                              g_pending_x.time);
        xcb_flush(g_xcb);
        finish_pending_x();
        return;
    }

    size_t cap = 1024;
    char *body = malloc(cap);
    if (!body) {
        finish_pending_x();
        return;
    }
    size_t off = 0;

    if (gnome_format) {
        /* "copy\n" prefix tells Nautilus this is a copy, not a cut. */
        const char *act = "copy\n";
        size_t al = strlen(act);
        memcpy(body + off, act, al); off += al;
    }

    for (size_t i = 0; i < g_hdrop.n_uris; i++) {
        const char *uri = g_hdrop.uris[i];
        size_t ul = strlen(uri);
        size_t need = ul + 2;
        while (off + need >= cap) {
            cap *= 2;
            char *nb = realloc(body, cap);
            if (!nb) { free(body); finish_pending_x(); return; }
            body = nb;
        }
        memcpy(body + off, uri, ul); off += ul;
        bool last = (i + 1 == g_hdrop.n_uris);
        if (gnome_format) {
            /* Nautilus parses x-special/gnome-copied-files line-by-line
             * via g_strsplit("\n") — a trailing newline produces an
             * empty entry it interprets as a second (unknown) URI and
             * shows "error getting information about unknown" before
             * processing the real one. RFC 2483 doesn't apply here
             * (gnome-copied-files isn't text/uri-list) so leave the
             * separator OFF after the last URI. */
            if (!last) body[off++] = '\n';
        } else {
            /* text/uri-list per RFC 2483: every line, including last,
             * ends with CRLF. */
            body[off++] = '\r';
            body[off++] = '\n';
        }
    }

    xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE,
                        g_pending_x.requestor, g_pending_x.property,
                        g_pending_x.target, 8,
                        (uint32_t)off, body);
    send_selection_notify(g_pending_x.requestor, g_pending_x.selection,
                          g_pending_x.target, g_pending_x.property,
                          g_pending_x.time);
    xcb_flush(g_xcb);

    clip_log("served CF_HDROP (%zu URIs, %zu bytes, target=%s)",
             g_hdrop.n_uris, off,
             gnome_format ? "x-special/gnome-copied-files" : "text/uri-list");

    free(body);
    finish_pending_x();
}

/* Handle SelectionRequest by issuing FORMAT_DATA_REQ to host. */
static void handle_selection_request(xcb_selection_request_event_t *ev)
{
    /* MULTIPLE / TIMESTAMP / TARGETS are answered without going to host. */
    if (ev->target == a_TARGETS) {
        serve_targets(ev->requestor, ev->selection, ev->property, ev->time);
        return;
    }
    if (ev->target == a_TIMESTAMP) {
        uint32_t t = (uint32_t)ev->time;
        xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE, ev->requestor,
                            ev->property, XCB_ATOM_INTEGER, 32, 1, &t);
        send_selection_notify(ev->requestor, ev->selection, ev->target,
                              ev->property, ev->time);
        xcb_flush(g_xcb);
        return;
    }

    uint32_t cf = target_to_cf(ev->target);
    if (cf == 0) {
        send_selection_notify(ev->requestor, ev->selection, ev->target,
                              XCB_ATOM_NONE, ev->time);
        return;
    }

    /* If we already have a pending request, reject the new one. */
    if (g_pending_x.active || g_writer_fd < 0) {
        send_selection_notify(ev->requestor, ev->selection, ev->target,
                              XCB_ATOM_NONE, ev->time);
        return;
    }

    memset(&g_pending_x, 0, sizeof(g_pending_x));
    g_pending_x.active    = true;
    g_pending_x.requestor = ev->requestor;
    g_pending_x.property  = ev->property ? ev->property : ev->target;
    g_pending_x.target    = ev->target;
    g_pending_x.selection = ev->selection;
    g_pending_x.time      = ev->time;
    g_pending_x.cf        = cf;

    /* CF_HDROP needs streaming file collection — start a fresh tmpdir
     * BEFORE we send the request so we don't miss the first FILE_DATA. */
    if (cf == CF_HDROP) hdrop_collect_start();

    if (send_clip_msg(g_writer_fd, CLIP_MSG_FORMAT_DATA_REQ, cf, NULL, 0) < 0) {
        clip_log("send FORMAT_DATA_REQ failed");
        send_selection_notify(ev->requestor, ev->selection, ev->target,
                              XCB_ATOM_NONE, ev->time);
        finish_pending_x();
        return;
    }
    clip_log("X SelectionRequest target=%u cf=%u → host", ev->target, cf);
}

/* ---- Guest→host path: handle CLIPBOARD owner change ---- */

static void query_guest_targets(void)
{
    if (!g_targets_dirty || g_pending_host.active ||
        !g_sync_enabled || g_reader_fd < 0 ||
        g_selection_owner == XCB_NONE || g_selection_owner == g_window) return;

    g_targets_dirty = false;
    g_pending_host.active = true;
    g_pending_host.target = a_TARGETS;
    g_pending_host.property = a_TARGETS;
    g_pending_host.owner = g_selection_owner;
    g_pending_host.time = g_selection_time;
    xcb_delete_property(g_xcb, g_window, a_TARGETS);
    xcb_convert_selection(g_xcb, g_window, a_CLIPBOARD, a_TARGETS,
                          a_TARGETS, g_pending_host.time);
    xcb_flush(g_xcb);
    clip_log("CLIPBOARD owner changed, fetching TARGETS");
}

static bool pending_host_is_current(void)
{
    return g_pending_host.owner == g_selection_owner &&
           g_pending_host.time == g_selection_time && g_sync_enabled;
}

static void finish_pending_host(bool failed)
{
    if (failed && g_pending_host.active && g_pending_host.cf && g_reader_fd >= 0)
        send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_DATA_RESP,
                      g_pending_host.cf, NULL, 0);
    free(g_pending_host.accum);
    memset(&g_pending_host, 0, sizeof(g_pending_host));
    query_guest_targets();
}

static void handle_xfixes_selection_notify(xcb_xfixes_selection_notify_event_t *ev)
{
    if (ev->selection != a_CLIPBOARD) return;

    g_selection_owner = ev->owner;
    g_selection_time = ev->selection_timestamp;
    g_guest_format_count = 0;
    g_targets_dirty = g_sync_enabled && g_reader_fd >= 0 &&
                      ev->owner != XCB_NONE && ev->owner != g_window;
    /* Finish the old transfer before reusing its reply property. */
    query_guest_targets();
}

/* SelectionNotify arrives in response to xcb_convert_selection — could
 * be the TARGETS reply (and we then need to ship FORMAT_LIST), or the
 * data reply for a specific format. */
static void handle_selection_notify(xcb_selection_notify_event_t *ev)
{
    if (!g_pending_host.active || ev->requestor != g_window ||
        ev->selection != a_CLIPBOARD || ev->target != g_pending_host.target ||
        ev->time != g_pending_host.time ||
        (ev->property != XCB_ATOM_NONE && ev->property != g_pending_host.property))
        return;

    if (ev->property == XCB_ATOM_NONE) {
        clip_log("SelectionNotify with no property — owner declined");
        finish_pending_host(true);
        return;
    }

    /* Read the property contents from our own window. */
    xcb_get_property_cookie_t pc = xcb_get_property(g_xcb, 1, g_window,
                                                    ev->property, XCB_GET_PROPERTY_TYPE_ANY,
                                                    0, UINT32_MAX);
    xcb_get_property_reply_t *pr = xcb_get_property_reply(g_xcb, pc, NULL);
    if (!pr) {
        finish_pending_host(true);
        return;
    }

    if (pr->type == a_INCR) {
        /* INCR transfer starts — property events on our window will
         * deliver the chunks. */
        if (!g_pending_host.accum) {
            g_pending_host.accum_cap = 64 * 1024;
            g_pending_host.accum = malloc(g_pending_host.accum_cap);
            g_pending_host.accum_len = 0;
        }
        g_pending_host.incr = true;
        free(pr);
        return;
    }

    uint32_t n = xcb_get_property_value_length(pr);
    void *val = xcb_get_property_value(pr);

    if (!pending_host_is_current() || pr->type == XCB_ATOM_NONE) {
        free(pr);
        finish_pending_host(true);
        return;
    }

    if (ev->target == a_TARGETS) {
        if (pr->type != a_ATOM || pr->format != 32) {
            free(pr);
            finish_pending_host(true);
            return;
        }
        /* TARGETS arrived — build a FORMAT_LIST and ship. */
        const xcb_atom_t *atoms = (const xcb_atom_t *)val;
        uint32_t n_atoms = n / 4;
        uint8_t buf[16384];
        uint32_t count = 0;
        size_t off = 4;
        g_guest_format_count = 0;
        for (uint32_t i = 0; i < n_atoms; i++) {
            uint32_t cf = target_to_cf(atoms[i]);
            if (cf == 0 || guest_target_for_cf(cf) != XCB_ATOM_NONE) continue;
            if (off + 8 > sizeof(buf)) break;
            g_guest_formats[count].cf = cf;
            g_guest_formats[count].target = cf_to_offered_target(cf, atoms, n_atoms);
            g_guest_format_count = count + 1;
            *(uint32_t *)(buf + off) = cf;       off += 4;
            *(uint32_t *)(buf + off) = 0;        off += 4;   /* name_len, only used for registered names */
            count++;
            if (count >= CLIP_MAX_FORMATS) break;
        }
        *(uint32_t *)buf = count;
        if (count > 0 && g_reader_fd >= 0) {
            send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_LIST, 0,
                           buf, (uint32_t)off);
            clip_log("→ host FORMAT_LIST (%u formats)", count);
        }
        free(pr);
        finish_pending_host(false);
        return;
    }

    /* Otherwise this is data for a specific format the host previously
     * asked for via FORMAT_DATA_REQ. Use the CF we recorded when issuing
     * the request — target_to_cf() would collapse CF_DIBV5 → CF_DIB
     * because both map to the same X target. */
    uint32_t cf = g_pending_host.cf;
    if (cf == 0 || g_reader_fd < 0) {
        free(pr);
        finish_pending_host(true);
        return;
    }

    if (cf == CF_HDROP) {
        /* The X reply is a URI list. Don't pass through — translate to
         * the host's FILE_DATA stream so Windows can paste actual
         * file contents (mirror of appsandbox-clipboard-reader.c
         * CF_HDROP branch). */
        deliver_hdrop_to_host((const uint8_t *)val, n);
        free(pr);
        finish_pending_host(false);
        return;
    }

    /* Convert and reply. */
    size_t cf_len = 0;
    uint8_t *cf_bytes = convert_x_to_cf(cf, val, n, &cf_len);
    if (cf_bytes) {
        send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_DATA_RESP, cf,
                       cf_bytes, (uint32_t)cf_len);
        free(cf_bytes);
        clip_log("→ host FORMAT_DATA_RESP cf=%u (%zu bytes)", cf, cf_len);
    } else {
        send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_DATA_RESP, cf, NULL, 0);
        clip_log("→ host FORMAT_DATA_RESP cf=%u (empty, convert failed)", cf);
    }
    free(pr);
    finish_pending_host(false);
}

/* PropertyNotify routing:
 *  - On the requestor's window, state=Delete: they consumed our INCR
 *    chunk — send the next one (or the EOF marker).
 *  - On our own window, state=NewValue: a Mutter→us INCR chunk
 *    arrived; accumulate it. */
static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    /* Outbound INCR (host→guest paste): requestor deleted our chunk. */
    if (g_pending_x.active && g_pending_x.incr &&
        ev->window == g_pending_x.requestor &&
        ev->atom   == g_pending_x.property &&
        ev->state  == XCB_PROPERTY_DELETE) {
        send_next_incr_chunk();
        return;
    }

    if (ev->window != g_window) return;
    if (ev->state != XCB_PROPERTY_NEW_VALUE) return;
    if (!g_pending_host.active || !g_pending_host.incr ||
        ev->atom != g_pending_host.property) return;

    xcb_get_property_cookie_t pc = xcb_get_property(g_xcb, 1, g_window,
                                                    ev->atom, XCB_GET_PROPERTY_TYPE_ANY,
                                                    0, UINT32_MAX);
    xcb_get_property_reply_t *pr = xcb_get_property_reply(g_xcb, pc, NULL);
    if (!pr) {
        finish_pending_host(true);
        return;
    }
    if (pr->type == XCB_ATOM_NONE) {
        free(pr);
        return;
    }
    uint32_t n = xcb_get_property_value_length(pr);
    void *val = xcb_get_property_value(pr);

    if (n == 0) {
        /* End of INCR stream — ship accumulated bytes. Prefer the CF
         * we recorded when issuing the request (target_to_cf would
         * collapse DIBV5 → DIB). */
        uint32_t cf = g_pending_host.cf ? g_pending_host.cf
                                        : target_to_cf(g_pending_host.target);
        bool replied = false;
        if (pending_host_is_current() && cf == CF_HDROP &&
            g_reader_fd >= 0 && g_pending_host.accum) {
            deliver_hdrop_to_host(g_pending_host.accum,
                                  g_pending_host.accum_len);
            replied = true;
        } else if (pending_host_is_current() && cf &&
                   g_reader_fd >= 0 && g_pending_host.accum) {
            size_t cf_len = 0;
            uint8_t *cf_bytes = convert_x_to_cf(cf, g_pending_host.accum,
                                                 g_pending_host.accum_len, &cf_len);
            if (cf_bytes) {
                send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_DATA_RESP, cf,
                               cf_bytes, (uint32_t)cf_len);
                free(cf_bytes);
                clip_log("→ host (INCR) FORMAT_DATA_RESP cf=%u (%zu bytes)",
                         cf, cf_len);
                replied = true;
            }
        }
        free(pr);
        xcb_delete_property(g_xcb, g_window, ev->atom);
        xcb_flush(g_xcb);
        finish_pending_host(!replied);
        return;
    } else {
        /* Accumulate. */
        if (g_pending_host.accum_len + n > g_pending_host.accum_cap) {
            /* Grow into a temporary capacity and commit accum/accum_cap only on
               a successful realloc. The original doubled accum_cap *before* the
               realloc, so on realloc failure accum still pointed at the old,
               smaller buffer while accum_cap was already inflated -> the memcpy
               below overflowed the un-enlarged buffer (C10). */
            size_t new_cap = g_pending_host.accum_cap ? g_pending_host.accum_cap : 1;
            while (g_pending_host.accum_len + n > new_cap)
                new_cap *= 2;
            uint8_t *no = realloc(g_pending_host.accum, new_cap);
            if (no) {
                g_pending_host.accum = no;
                g_pending_host.accum_cap = new_cap;
            }
        }
        /* Append only if the buffer truly has room; if realloc failed above the
           capacity is unchanged (still too small) so the chunk is dropped
           rather than overflowing. */
        if (g_pending_host.accum &&
            g_pending_host.accum_len + n <= g_pending_host.accum_cap) {
            memcpy(g_pending_host.accum + g_pending_host.accum_len, val, n);
            g_pending_host.accum_len += n;
        }
    }
    free(pr);
    /* Delete the property to signal we've consumed this chunk. */
    xcb_delete_property(g_xcb, g_window, ev->atom);
    xcb_flush(g_xcb);
}

/* ============================================================
 *   Vsock channel handlers
 * ============================================================ */

static void take_clipboard_ownership(xcb_timestamp_t time)
{
    g_guest_format_count = 0;
    g_selection_owner = g_window;
    g_selection_time = time;
    g_targets_dirty = false;
    xcb_set_selection_owner(g_xcb, g_window, a_CLIPBOARD, time);
    xcb_flush(g_xcb);
    g_we_own_clipboard = true;
}

static void release_clipboard_ownership(void)
{
    if (!g_we_own_clipboard) return;
    g_guest_format_count = 0;
    g_selection_owner = XCB_NONE;
    g_targets_dirty = false;
    xcb_set_selection_owner(g_xcb, XCB_NONE, a_CLIPBOARD, XCB_CURRENT_TIME);
    xcb_flush(g_xcb);
    g_we_own_clipboard = false;
}

/* Writer channel (host→guest direction) — host pushes its clipboard
 * to us. We become CLIPBOARD owner so local apps can paste it. */
static int handle_writer_message(int fd, const struct ClipHeader *h)
{
    switch (h->msg_type) {

    case CLIP_MSG_SYNC_ENABLE: {
        uint8_t flag = 0;
        if (h->data_size >= 1) {
            if (recv_exact(fd, &flag, 1) < 0) return -1;
            uint32_t rest = h->data_size - 1;
            while (rest > 0) {
                uint8_t skip[256];
                size_t c = rest > sizeof(skip) ? sizeof(skip) : rest;
                if (recv_exact(fd, skip, c) < 0) return -1;
                rest -= (uint32_t)c;
            }
        }
        g_sync_enabled = flag ? true : false;
        clip_log("recv SYNC_ENABLE=%d", flag);
        if (!g_sync_enabled) {
            g_guest_format_count = 0;
            g_targets_dirty = false;
            release_clipboard_ownership();
        }
        return 0;
    }

    case CLIP_MSG_FORMAT_LIST: {
        /* Need at least the 4-byte leading count; reject short frames so the
           `*(uint32_t *)buf` read below cannot over-read the allocation (L11,
           Linux analog of H4/C9). */
        if (h->data_size < 4 || h->data_size > 16384) return -1;
        uint8_t *buf = malloc(h->data_size);
        if (!buf) return -1;
        if (recv_exact(fd, buf, h->data_size) < 0) { free(buf); return -1; }

        if (!g_sync_enabled) { free(buf); return 0; }

        /* Parse: uint32 count; per-format { uint32 fmt_id; uint32 name_len; char name[]; } */
        uint32_t count = *(uint32_t *)buf;
        if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
        size_t off = 4;
        g_host_format_count = 0;
        for (uint32_t i = 0; i < count && off + 8 <= h->data_size; i++) {
            uint32_t fmt_id   = *(uint32_t *)(buf + off); off += 4;
            uint32_t name_len = *(uint32_t *)(buf + off); off += 4;

            /* Detect "HTML Format" registered name and remember its id. */
            if (name_len > 0 && off + name_len <= h->data_size) {
                if (name_len == 11 &&
                    memcmp(buf + off, "HTML Format", 11) == 0) {
                    g_cf_html_id = fmt_id;
                }
            }
            off += name_len;

            g_host_formats[g_host_format_count++] = fmt_id;
        }
        free(buf);

        take_clipboard_ownership(XCB_CURRENT_TIME);
        clip_log("recv FORMAT_LIST (%d formats); now own CLIPBOARD",
                 g_host_format_count);

        /* Consume the host's one-shot reader_suppress flag.
         *
         * vm_clipboard.c:501 sets reader_suppress=1 every time it
         * pushes us a FORMAT_LIST, expecting the guest to bounce one
         * straight back so the flag clears (vm_clipboard.c:811). On
         * the Windows guest that happens naturally — SetClipboardData
         * in the writer process fires WM_CLIPBOARDUPDATE which the
         * separate reader process picks up and ships back. Our helper
         * is one process and filters self-owner XFIXES events to
         * avoid loops, so without this explicit echo the flag stays
         * set and the next real guest-clipboard change gets dropped
         * with "FORMAT_LIST suppressed (echo from host->guest)". */
        if (g_reader_fd >= 0 && g_host_format_count > 0) {
            uint8_t echo[16384];
            size_t eoff = 4;
            uint32_t ecount = 0;
            for (int i = 0; i < g_host_format_count &&
                            eoff + 8 <= sizeof(echo); i++) {
                *(uint32_t *)(echo + eoff) = g_host_formats[i]; eoff += 4;
                *(uint32_t *)(echo + eoff) = 0;                  eoff += 4;
                ecount++;
            }
            *(uint32_t *)echo = ecount;
            send_clip_msg(g_reader_fd, CLIP_MSG_FORMAT_LIST, 0,
                          echo, (uint32_t)eoff);
        }
        return 0;
    }

    case CLIP_MSG_FORMAT_DATA_RESP: {
        uint32_t cf = h->format;
        if (h->data_size > CLIP_MAX_PAYLOAD) return -1;
        uint8_t *body = NULL;
        if (h->data_size > 0) {
            body = malloc(h->data_size);
            if (!body) return -1;
            if (recv_exact(fd, body, h->data_size) < 0) { free(body); return -1; }
        }
        /* CF_HDROP: the empty FORMAT_DATA_RESP is the end-of-stream
         * marker for the preceding FILE_DATA messages. Hand off to the
         * URI-list builder instead of the generic byte deliverer. */
        if (cf == CF_HDROP && g_hdrop.active &&
            g_pending_x.active && g_pending_x.cf == CF_HDROP) {
            deliver_hdrop_to_x();
        } else if (g_pending_x.active && g_pending_x.cf == cf) {
            deliver_host_data_to_x(body, h->data_size);
        }
        free(body);
        return 0;
    }

    case CLIP_MSG_FILE_DATA: {
        /* Wire layout per ClipFileInfo + path bytes (the ClipHeader's
         * data_size counts JUST those), followed by file_size bytes of
         * raw file content (NOT counted in data_size — has to be drained
         * separately or we desync the protocol). */
        if (h->data_size < sizeof(struct ClipFileInfo)) return -1;

        struct ClipFileInfo fi;
        if (recv_exact(fd, &fi, sizeof(fi)) < 0) return -1;

        uint32_t path_bytes = h->data_size - (uint32_t)sizeof(fi);
        uint8_t *path_utf16 = NULL;
        if (path_bytes > 0) {
            path_utf16 = malloc(path_bytes);
            if (!path_utf16) return -1;
            if (recv_exact(fd, path_utf16, path_bytes) < 0) {
                free(path_utf16); return -1;
            }
        }

        bool collect = g_hdrop.active &&
                       g_pending_x.active &&
                       g_pending_x.cf == CF_HDROP &&
                       g_hdrop.tmp_dir[0] != '\0';

        char *u8 = NULL;
        if (collect && path_bytes > 0) {
            size_t ul = 0;
            uint8_t *u8b = utf16le_to_utf8(path_utf16, path_bytes, &ul);
            if (u8b) {
                u8 = malloc(ul + 1);
                if (u8) { memcpy(u8, u8b, ul); u8[ul] = '\0'; }
                free(u8b);
            }
        }
        free(path_utf16);

        bool wrote_locally = false;

        if (collect && u8 && path_safe(u8)) {
            /* Windows uses backslashes; convert to forward slashes for
             * Linux. */
            for (char *p = u8; *p; p++) if (*p == '\\') *p = '/';

            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", g_hdrop.tmp_dir, u8);

            if (fi.is_directory) {
                int mk = mkdir_p(full);   /* 0 on success/EEXIST, -1 on failure */
                /* Directories carry no body bytes (file_size==0). A *top-level*
                   directory (no '/' in its relative path) is itself a dropped
                   item, so list its URI — the file manager then copies the whole
                   folder recursively. Nested dirs/files are reached through it
                   and must NOT be listed (listing every nested file is what made
                   a pasted folder dump its contents flat). Only list it if the
                   dir actually exists (mk==0), mirroring the file branch's
                   wrote_locally guard so a failed mkdir can't leave a URI that
                   points at a missing folder. */
                if (mk == 0 && !strchr(u8, '/')) {
                    char uri[1100];
                    snprintf(uri, sizeof(uri), "file://%s", full);
                    hdrop_add_uri(uri);
                }
            } else {
                /* Ensure parent exists. */
                char parent[1024];
                snprintf(parent, sizeof(parent), "%s", full);
                char *slash = strrchr(parent, '/');
                if (slash && slash != parent) {
                    *slash = '\0';
                    mkdir_p(parent);
                }

                FILE *fp = fopen(full, "wb");
                if (!fp) clip_log("hdrop: fopen(%s): %s", full, strerror(errno));

                uint64_t remaining = fi.file_size;
                uint8_t buf[64 * 1024];
                while (remaining > 0) {
                    size_t c = remaining > sizeof(buf) ? sizeof(buf)
                                                       : (size_t)remaining;
                    if (recv_exact(fd, buf, c) < 0) {
                        if (fp) fclose(fp);
                        free(u8);
                        return -1;
                    }
                    if (fp) {
                        size_t w = fwrite(buf, 1, c, fp);
                        if (w != c) clip_log("hdrop: fwrite short");
                    }
                    remaining -= c;
                }
                if (fp) { fclose(fp); wrote_locally = true; }

                /* Only list TOP-LEVEL files. A file nested inside a dropped
                   folder is reached via that folder's URI (added above); listing
                   it here too made the file manager paste it flat at the target,
                   losing the folder structure. */
                if (wrote_locally && !strchr(u8, '/')) {
                    char uri[1100];
                    snprintf(uri, sizeof(uri), "file://%s", full);
                    hdrop_add_uri(uri);
                }
            }
        } else {
            /* Drain file body to keep the stream in sync. */
            uint64_t remaining = fi.file_size;
            uint8_t skip[4096];
            while (remaining > 0) {
                size_t c = remaining > sizeof(skip) ? sizeof(skip)
                                                   : (size_t)remaining;
                if (recv_exact(fd, skip, c) < 0) {
                    free(u8); return -1;
                }
                remaining -= c;
            }
        }

        free(u8);
        return 0;
    }

    default:
        if (h->data_size > 0) {
            uint8_t skip[256];
            uint32_t rem = h->data_size;
            while (rem > 0) {
                size_t c = rem > sizeof(skip) ? sizeof(skip) : rem;
                if (recv_exact(fd, skip, c) < 0) return -1;
                rem -= (uint32_t)c;
            }
        }
        return 0;
    }
}

/* Reader channel (guest→host direction) — host requests data we
 * previously announced. We XConvertSelection on the local CLIPBOARD. */
static int handle_reader_message(int fd, const struct ClipHeader *h)
{
    switch (h->msg_type) {

    case CLIP_MSG_FORMAT_DATA_REQ: {
        if (g_pending_host.active) {
            /* Reject; host expects a reply. */
            send_clip_msg(fd, CLIP_MSG_FORMAT_DATA_RESP, h->format, NULL, 0);
            return 0;
        }
        xcb_atom_t target = guest_target_for_cf(h->format);
        if (target == XCB_ATOM_NONE) {
            send_clip_msg(fd, CLIP_MSG_FORMAT_DATA_RESP, h->format, NULL, 0);
            return 0;
        }
        g_pending_host.active = true;
        g_pending_host.cf     = h->format;
        g_pending_host.target = target;
        g_pending_host.property = a_CLIP_DATA;
        g_pending_host.owner = g_selection_owner;
        g_pending_host.time = g_selection_time;
        g_pending_host.incr   = false;
        g_pending_host.accum  = NULL;
        g_pending_host.accum_len = 0;
        g_pending_host.accum_cap = 0;
        xcb_delete_property(g_xcb, g_window, a_CLIP_DATA);
        xcb_convert_selection(g_xcb, g_window, a_CLIPBOARD, target,
                              a_CLIP_DATA, g_pending_host.time);
        xcb_flush(g_xcb);
        clip_log("recv FORMAT_DATA_REQ cf=%u → XConvertSelection target=%u",
                 h->format, target);
        return 0;
    }

    default:
        if (h->data_size > 0) {
            uint8_t skip[256];
            uint32_t rem = h->data_size;
            while (rem > 0) {
                size_t c = rem > sizeof(skip) ? sizeof(skip) : rem;
                if (recv_exact(fd, skip, c) < 0) return -1;
                rem -= (uint32_t)c;
            }
        }
        return 0;
    }
}

/* ============================================================
 *   epoll loop
 * ============================================================ */

static int epoll_add(int ep, int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}
static int epoll_del(int ep, int fd)
{
    return epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
}

static void process_x_events(void)
{
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(g_xcb)) != NULL) {
        uint8_t type = ev->response_type & ~0x80;
        if (type == g_xfixes_event_base + XCB_XFIXES_SELECTION_NOTIFY) {
            handle_xfixes_selection_notify((xcb_xfixes_selection_notify_event_t *)ev);
        } else switch (type) {
        case XCB_SELECTION_REQUEST:
            handle_selection_request((xcb_selection_request_event_t *)ev);
            break;
        case XCB_SELECTION_NOTIFY:
            handle_selection_notify((xcb_selection_notify_event_t *)ev);
            break;
        case XCB_PROPERTY_NOTIFY:
            handle_property_notify((xcb_property_notify_event_t *)ev);
            break;
        case XCB_SELECTION_CLEAR:
            /* Another app took CLIPBOARD ownership from us. */
            g_we_own_clipboard = false;
            clip_log("SelectionClear — another owner took CLIPBOARD");
            break;
        case 0: {
            xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
            clip_log("X error: code=%u major=%u minor=%u",
                     err->error_code, err->major_code, err->minor_code);
            break;
        }
        default:
            break;
        }
        free(ev);
    }
}

int main(void)
{
    log_open();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    clip_log("appsandbox-clipboard starting (pid=%d)", getpid());

    if (xcb_setup() < 0) return 1;

    /* Inherit pre-bound listen sockets from the agent if we were
     * launched as its helper (systemd LISTEN_FDS / LISTEN_PID
     * convention). Otherwise bind ourselves — requires CAP_NET_BIND_SERVICE
     * since vsock <1024 is privileged, and standalone use is for dev
     * iteration only. */
    int listen_writer = -1, listen_reader = -1;
    const char *lf  = getenv("LISTEN_FDS");
    const char *lpid = getenv("LISTEN_PID");
    if (lf && lpid && atoi(lf) >= 2 && atoi(lpid) == (int)getpid()) {
        /* fd 3 = writer (port 5), fd 4 = reader (port 6) — order set
         * by the agent's spawn_clipboard_helper(). */
        listen_writer = 3;
        listen_reader = 4;
        unsetenv("LISTEN_FDS");
        unsetenv("LISTEN_PID");
        clip_log("using inherited fds 3/4 from parent");
    } else {
        listen_writer = vsock_listen(VSOCK_PORT_WRITER);
        listen_reader = vsock_listen(VSOCK_PORT_READER);
    }
    if (listen_writer < 0 || listen_reader < 0) return 1;
    clip_log("listening on vsock :%u (writer) and :%u (reader)",
             VSOCK_PORT_WRITER, VSOCK_PORT_READER);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) { clip_log("epoll_create1: %s", strerror(errno)); return 1; }
    int xcb_fd = xcb_get_file_descriptor(g_xcb);
    epoll_add(ep, listen_writer, EPOLLIN);
    epoll_add(ep, listen_reader, EPOLLIN);
    epoll_add(ep, xcb_fd, EPOLLIN);

    while (!g_stop) {
        struct epoll_event evs[8];
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            clip_log("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;

            if (fd == listen_writer) {
                int c = accept(listen_writer, NULL, NULL);
                if (c < 0) continue;
                if (g_writer_fd >= 0) close(g_writer_fd);
                g_writer_fd = c;
                uint32_t ready = CLIP_READY_MAGIC;
                send_exact(c, &ready, 4);
                epoll_add(ep, c, EPOLLIN);
                clip_log("writer channel accepted");
            }
            else if (fd == listen_reader) {
                int c = accept(listen_reader, NULL, NULL);
                if (c < 0) continue;
                if (g_reader_fd >= 0) close(g_reader_fd);
                g_reader_fd = -1;
                g_guest_format_count = 0;
                finish_pending_host(false);
                g_targets_dirty = false;
                g_reader_fd = c;
                uint32_t ready = CLIP_READY_MAGIC;
                send_exact(c, &ready, 4);
                epoll_add(ep, c, EPOLLIN);
                clip_log("reader channel accepted");
            }
            else if (fd == xcb_fd) {
                process_x_events();
            }
            else if (fd == g_writer_fd) {
                struct ClipHeader h;
                if (recv_exact(fd, &h, sizeof(h)) < 0 ||
                    h.magic != CLIP_MAGIC ||
                    handle_writer_message(fd, &h) < 0) {
                    clip_log("writer channel closed");
                    epoll_del(ep, fd);
                    close(fd);
                    g_writer_fd = -1;
                    release_clipboard_ownership();
                    g_sync_enabled = false;
                    g_guest_format_count = 0;
                    g_targets_dirty = false;
                    /* Abandon any in-flight host roundtrip so SelectionRequests
                     * don't stay rejected forever. */
                    if (g_pending_x.active) {
                        send_selection_notify(g_pending_x.requestor,
                                              g_pending_x.selection,
                                              g_pending_x.target,
                                              XCB_ATOM_NONE,
                                              g_pending_x.time);
                        finish_pending_x();
                        xcb_flush(g_xcb);
                    }
                }
            }
            else if (fd == g_reader_fd) {
                struct ClipHeader h;
                if (recv_exact(fd, &h, sizeof(h)) < 0 ||
                    h.magic != CLIP_MAGIC ||
                    handle_reader_message(fd, &h) < 0) {
                    clip_log("reader channel closed");
                    epoll_del(ep, fd);
                    close(fd);
                    g_reader_fd = -1;
                    /* Drop any in-flight X→host fetch state. */
                    g_guest_format_count = 0;
                    finish_pending_host(false);
                    g_targets_dirty = false;
                }
            }
        }
    }

    clip_log("shutting down");
    if (g_writer_fd >= 0) close(g_writer_fd);
    if (g_reader_fd >= 0) close(g_reader_fd);
    close(listen_writer);
    close(listen_reader);
    if (g_xcb) xcb_disconnect(g_xcb);
    if (g_log) fclose(g_log);
    return 0;
}
