/*
 * appsandbox-input.c — Linux input injection for AppSandbox.
 *
 * Listens on AF_VSOCK port 3, sends IRDY on accept, and translates
 * incoming ASIN InputPackets into /dev/uinput events. The host stack
 * already speaks this protocol (see src/backend_win/vm_display_idd.c).
 *
 * Primary virtual device exposing absolute pointer, wheel, buttons, and
 * full keyboard. GNOME / Mutter (Wayland) and Xorg both pick it up via
 * libinput automatically because uinput presents a real evdev node.
 *
 * Wire protocol: src/core/protocol.h.
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
#include <endian.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/uinput.h>
#include <linux/vm_sockets.h>
#include <systemd/sd-login.h>
#include "protocol.h"

#define VSOCK_PORT          3

/* Absolute pointer range. We don't know the guest screen size before
 * accept, and it can change mid-session. We pick a coordinate space
 * large enough for any plausible resolution and trust libinput to map
 * it to the active screen. 32767 is the de-facto absolute-tablet range. */
#define ABS_RANGE   32767

static volatile sig_atomic_t g_stop = 0;
static int g_frame_w = 1920;
static int g_frame_h = 1080;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void in_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[input] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- VK → Linux keycode table ----
 * Windows VK values are 8-bit; keys we don't map fall through to 0
 * (which uinput silently drops). Reference KEY_* values come from
 * <linux/input-event-codes.h>. */

#include <linux/input-event-codes.h>

static const uint16_t g_vk_to_key[256] = {
    [0x08] = KEY_BACKSPACE,
    [0x09] = KEY_TAB,
    [0x0D] = KEY_ENTER,
    [0x10] = KEY_LEFTSHIFT,
    [0x11] = KEY_LEFTCTRL,
    [0x12] = KEY_LEFTALT,
    [0x13] = KEY_PAUSE,
    [0x14] = KEY_CAPSLOCK,
    [0x1B] = KEY_ESC,
    [0x20] = KEY_SPACE,
    [0x21] = KEY_PAGEUP,
    [0x22] = KEY_PAGEDOWN,
    [0x23] = KEY_END,
    [0x24] = KEY_HOME,
    [0x25] = KEY_LEFT,
    [0x26] = KEY_UP,
    [0x27] = KEY_RIGHT,
    [0x28] = KEY_DOWN,
    [0x2C] = KEY_SYSRQ,    /* PrintScreen */
    [0x2D] = KEY_INSERT,
    [0x2E] = KEY_DELETE,
    [0x30] = KEY_0, [0x31] = KEY_1, [0x32] = KEY_2, [0x33] = KEY_3,
    [0x34] = KEY_4, [0x35] = KEY_5, [0x36] = KEY_6, [0x37] = KEY_7,
    [0x38] = KEY_8, [0x39] = KEY_9,
    [0x41] = KEY_A, [0x42] = KEY_B, [0x43] = KEY_C, [0x44] = KEY_D,
    [0x45] = KEY_E, [0x46] = KEY_F, [0x47] = KEY_G, [0x48] = KEY_H,
    [0x49] = KEY_I, [0x4A] = KEY_J, [0x4B] = KEY_K, [0x4C] = KEY_L,
    [0x4D] = KEY_M, [0x4E] = KEY_N, [0x4F] = KEY_O, [0x50] = KEY_P,
    [0x51] = KEY_Q, [0x52] = KEY_R, [0x53] = KEY_S, [0x54] = KEY_T,
    [0x55] = KEY_U, [0x56] = KEY_V, [0x57] = KEY_W, [0x58] = KEY_X,
    [0x59] = KEY_Y, [0x5A] = KEY_Z,
    [0x5B] = KEY_LEFTMETA,  /* Left Win */
    [0x5C] = KEY_RIGHTMETA, /* Right Win */
    [0x5D] = KEY_COMPOSE,   /* App / Menu */
    [0x60] = KEY_KP0, [0x61] = KEY_KP1, [0x62] = KEY_KP2, [0x63] = KEY_KP3,
    [0x64] = KEY_KP4, [0x65] = KEY_KP5, [0x66] = KEY_KP6, [0x67] = KEY_KP7,
    [0x68] = KEY_KP8, [0x69] = KEY_KP9,
    [0x6A] = KEY_KPASTERISK,
    [0x6B] = KEY_KPPLUS,
    [0x6D] = KEY_KPMINUS,
    [0x6E] = KEY_KPDOT,
    [0x6F] = KEY_KPSLASH,
    [0x70] = KEY_F1,  [0x71] = KEY_F2,  [0x72] = KEY_F3,  [0x73] = KEY_F4,
    [0x74] = KEY_F5,  [0x75] = KEY_F6,  [0x76] = KEY_F7,  [0x77] = KEY_F8,
    [0x78] = KEY_F9,  [0x79] = KEY_F10, [0x7A] = KEY_F11, [0x7B] = KEY_F12,
    [0x90] = KEY_NUMLOCK,
    [0x91] = KEY_SCROLLLOCK,
    [0xA0] = KEY_LEFTSHIFT,  [0xA1] = KEY_RIGHTSHIFT,
    [0xA2] = KEY_LEFTCTRL,   [0xA3] = KEY_RIGHTCTRL,
    [0xA4] = KEY_LEFTALT,    [0xA5] = KEY_RIGHTALT,
    [0xBA] = KEY_SEMICOLON,
    [0xBB] = KEY_EQUAL,
    [0xBC] = KEY_COMMA,
    [0xBD] = KEY_MINUS,
    [0xBE] = KEY_DOT,
    [0xBF] = KEY_SLASH,
    [0xC0] = KEY_GRAVE,
    [0xDB] = KEY_LEFTBRACE,
    [0xDC] = KEY_BACKSLASH,
    [0xDD] = KEY_RIGHTBRACE,
    [0xDE] = KEY_APOSTROPHE,
};

static const uint16_t g_set1_extended[128] = {
    [0x10] = KEY_PREVIOUSSONG, [0x19] = KEY_NEXTSONG,
    [0x1C] = KEY_KPENTER, [0x1D] = KEY_RIGHTCTRL,
    [0x20] = KEY_MUTE, [0x21] = KEY_CALC, [0x22] = KEY_PLAYPAUSE,
    [0x24] = KEY_STOPCD, [0x2E] = KEY_VOLUMEDOWN,
    [0x30] = KEY_VOLUMEUP, [0x32] = KEY_HOMEPAGE,
    [0x35] = KEY_KPSLASH, [0x36] = KEY_RIGHTSHIFT,
    [0x37] = KEY_SYSRQ, [0x38] = KEY_RIGHTALT,
    [0x45] = KEY_NUMLOCK, [0x46] = KEY_PAUSE,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP,
    [0x4B] = KEY_LEFT, [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x5B] = KEY_LEFTMETA, [0x5C] = KEY_RIGHTMETA,
    [0x5D] = KEY_COMPOSE, [0x5E] = KEY_POWER, [0x5F] = KEY_SLEEP,
    [0x63] = KEY_WAKEUP, [0x65] = KEY_SEARCH,
    [0x66] = KEY_BOOKMARKS, [0x67] = KEY_REFRESH,
    [0x68] = KEY_STOP, [0x69] = KEY_FORWARD, [0x6A] = KEY_BACK,
    [0x6B] = KEY_COMPUTER, [0x6C] = KEY_MAIL,
    [0x6D] = KEY_MEDIA, [0x6E] = KEY_PROG1, [0x6F] = KEY_PROG2,
};

static uint16_t physical_keycode(uint32_t scan, uint32_t flags)
{
    if (scan == 0xF1) return KEY_HANJA;
    if (scan == 0xF2) return KEY_HANGEUL;
    if (flags & INPUT_KEY_EXTENDED)
        return scan < 128 ? g_set1_extended[scan] : 0;
    /* Linux retains the set-1 values for the original PC keyboard keys. */
    if ((scan >= 0x01 && scan <= 0x53) || (scan >= 0x56 && scan <= 0x58))
        return (uint16_t)scan;
    if (scan >= 0x64 && scan <= 0x6F)
        return (uint16_t)(KEY_F13 + scan - 0x64);
    switch (scan) {
    case 0x54: return KEY_SYSRQ;
    case 0x59: return KEY_KPEQUAL;
    case 0x5C: return KEY_KPJPCOMMA;
    case 0x70: return KEY_KATAKANAHIRAGANA;
    case 0x71: case 0xF1: return KEY_HANJA;
    case 0x72: case 0xF2: return KEY_HANGEUL;
    case 0x73: return KEY_RO;
    case 0x76: return KEY_F24;
    case 0x77: return KEY_HIRAGANA;
    case 0x78: return KEY_KATAKANA;
    case 0x79: return KEY_HENKAN;
    case 0x7B: return KEY_MUHENKAN;
    case 0x7D: return KEY_YEN;
    case 0x7E: return KEY_KPCOMMA;
    default: return 0;
    }
}

static uint16_t function_keycode(uint32_t vk)
{
    switch (vk) {
    case 0x03: case 0x13: return KEY_PAUSE;
    case 0x2C: return KEY_SYSRQ;
    case 0x5F: return KEY_SLEEP;
    case 0xA6: return KEY_BACK;
    case 0xA7: return KEY_FORWARD;
    case 0xA8: return KEY_REFRESH;
    case 0xA9: return KEY_STOP;
    case 0xAA: return KEY_SEARCH;
    case 0xAB: return KEY_BOOKMARKS;
    case 0xAC: return KEY_HOMEPAGE;
    case 0xAD: return KEY_MUTE;
    case 0xAE: return KEY_VOLUMEDOWN;
    case 0xAF: return KEY_VOLUMEUP;
    case 0xB0: return KEY_NEXTSONG;
    case 0xB1: return KEY_PREVIOUSSONG;
    case 0xB2: return KEY_STOPCD;
    case 0xB3: return KEY_PLAYPAUSE;
    case 0xB4: return KEY_MAIL;
    case 0xB5: return KEY_MEDIA;
    case 0xB6: return KEY_PROG1;
    case 0xB7: return KEY_PROG2;
    default: return 0;
    }
}

/* ---- uinput device setup ---- */

static int uinput_open(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        in_log("open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    /* Enable absolute pointer */
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    /* Pointer buttons + wheel */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);   /* hint to libinput: absolute pointer */
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    /* Keyboard — enable everything in the table that's nonzero */
    for (int i = 0; i < 256; i++) {
        if (g_vk_to_key[i])
            ioctl(fd, UI_SET_KEYBIT, g_vk_to_key[i]);
        uint16_t code = physical_keycode((uint32_t)i, 0);
        if (code) ioctl(fd, UI_SET_KEYBIT, code);
        code = physical_keycode((uint32_t)i, INPUT_KEY_EXTENDED);
        if (code) ioctl(fd, UI_SET_KEYBIT, code);
        code = function_keycode((uint32_t)i);
        if (code) ioctl(fd, UI_SET_KEYBIT, code);
    }

    /* Modern uinput setup (UI_DEV_SETUP) gives us absinfo per-axis */
    struct uinput_setup usetup = {0};
    strncpy(usetup.name, "AppSandbox Virtual Input", UINPUT_MAX_NAME_SIZE - 1);
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor  = 0xA53B;
    usetup.id.product = 0x0001;
    usetup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        in_log("UI_DEV_SETUP: %s", strerror(errno));
        close(fd); return -1;
    }

    struct uinput_abs_setup abs = {0};
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = ABS_RANGE;
    abs.code = ABS_X; ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_Y; ioctl(fd, UI_ABS_SETUP, &abs);

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        in_log("UI_DEV_CREATE: %s", strerror(errno));
        close(fd); return -1;
    }
    /* Let udev notice the new device before we start posting events */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
    nanosleep(&ts, NULL);
    return fd;
}

static void uinput_close(int fd)
{
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
    }
}

static int uinput_open_relative(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct uinput_setup setup = {0};
    strncpy(setup.name, "AppSandbox Relative Mouse", UINPUT_MAX_NAME_SIZE - 1);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0xA53B;
    setup.id.product = 0x0002;
    setup.id.version = 1;
    if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_Y) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0 ||
        ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
    nanosleep(&ts, NULL);
    return fd;
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev = {0};
    ev.type = type; ev.code = code; ev.value = value;
    /* Don't fail loudly on partial writes — keep streaming. */
    (void)!write(fd, &ev, sizeof(ev));
}

static void emit_syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

/* ---- Event translation ---- */

static void do_mouse_move(int ui_fd, uint32_t x, uint32_t y, int after_relative)
{
    /* Host gives us pixel coordinates in the current frame. Map to
     * 0..ABS_RANGE so the compositor scales correctly regardless of
     * the actual screen size. */
    int32_t ax = (int32_t)((uint64_t)x * ABS_RANGE / (g_frame_w ? g_frame_w : 1));
    int32_t ay = (int32_t)((uint64_t)y * ABS_RANGE / (g_frame_h ? g_frame_h : 1));
    if (ax < 0) ax = 0; if (ax > ABS_RANGE) ax = ABS_RANGE;
    if (ay < 0) ay = 0; if (ay > ABS_RANGE) ay = ABS_RANGE;
    if (after_relative) {
        /* EV_ABS filters unchanged values. Keep the reset in the same SYN frame. */
        emit(ui_fd, EV_ABS, ABS_X, ax ? ax - 1 : 1);
        emit(ui_fd, EV_ABS, ABS_Y, ay ? ay - 1 : 1);
    }
    emit(ui_fd, EV_ABS, ABS_X, ax);
    emit(ui_fd, EV_ABS, ABS_Y, ay);
    emit_syn(ui_fd);
}

static void do_mouse_button(int ui_fd, uint32_t btn_id, uint32_t down)
{
    uint16_t code = 0;
    switch (btn_id) {
    case INPUT_BTN_LEFT:   code = BTN_LEFT;   break;
    case INPUT_BTN_RIGHT:  code = BTN_RIGHT;  break;
    case INPUT_BTN_MIDDLE: code = BTN_MIDDLE; break;
    default: return;
    }
    emit(ui_fd, EV_KEY, code, down ? 1 : 0);
    emit_syn(ui_fd);
}

static void do_mouse_wheel(int ui_fd, int32_t delta)
{
    /* Windows wheel delta is ~120 per notch; REL_WHEEL is one tick per
     * notch. Round toward zero. */
    int32_t notches = delta / 120;
    if (notches == 0) notches = (delta > 0) - (delta < 0);
    emit(ui_fd, EV_REL, REL_WHEEL, notches);
    emit_syn(ui_fd);
}

static void do_key(int ui_fd, uint32_t vk, uint32_t scan, uint32_t flags)
{
    (void)scan;
    uint16_t code = g_vk_to_key[vk & 0xFF];
    if (!code) return;
    int is_up = (flags & 2) != 0;
    emit(ui_fd, EV_KEY, code, is_up ? 0 : 1);
    emit_syn(ui_fd);
}

static void release_physical_keys(int ui_fd, uint8_t held[KEY_CNT])
{
    int released = 0;
    for (unsigned code = 1; code < KEY_CNT; code++) {
        if (!held[code]) continue;
        emit(ui_fd, EV_KEY, (uint16_t)code, 0);
        held[code] = 0;
        released = 1;
    }
    if (released) emit_syn(ui_fd);
}

static void do_physical_key(int ui_fd, uint32_t vk, uint32_t scan,
                            uint32_t flags, uint8_t held[KEY_CNT])
{
    if (flags & ~(INPUT_KEY_EXTENDED | INPUT_KEY_UP)) return;
    uint16_t code = scan ? physical_keycode(scan, flags) : function_keycode(vk);
    if (!code) return;
    int up = (flags & INPUT_KEY_UP) != 0;
    if (up && !held[code]) return;
    emit(ui_fd, EV_KEY, code, up ? 0 : held[code] ? 2 : 1);
    held[code] = !up;
    emit_syn(ui_fd);
    /* Pause and Korean mode keys have no hardware break event. */
    if (!up && (code == KEY_PAUSE || code == KEY_HANGEUL || code == KEY_HANJA)) {
        emit(ui_fd, EV_KEY, code, 0);
        held[code] = 0;
        emit_syn(ui_fd);
    }
}

/* ---- vsock listener ---- */

static int vsock_listen(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { in_log("socket: %s", strerror(errno)); return -1; }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        in_log("bind :%u: %s", port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        in_log("listen: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static int recv_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = recv(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n; left -= (size_t)n;
    }
    return 0;
}

static int send_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int get_pointer_position(uint32_t *x, uint32_t *y)
{
    uid_t uid;
    if (sd_seat_get_active("seat0", NULL, &uid) < 0) return 0;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "/run/user/%u/appsandbox-pointer.sock", (unsigned)uid);
    struct stat st;
    if (lstat(addr.sun_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != uid) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return 0;
    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 || peer.uid != uid) {
        close(fd);
        return 0;
    }

    uint32_t position[4];
    size_t received = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (received < sizeof(position)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining = 100 - (now.tv_sec - start.tv_sec) * 1000 -
                         (now.tv_nsec - start.tv_nsec) / 1000000;
        if (remaining <= 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        ssize_t n = recv(fd, (char *)position + received, sizeof(position) - received, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;
        received += (size_t)n;
    }
    close(fd);
    if (received != sizeof(position)) return 0;
    for (unsigned i = 0; i < 4; i++) position[i] = le32toh(position[i]);
    if (!position[2] || !position[3] || position[2] > 16384 || position[3] > 16384 ||
        position[0] >= position[2] || position[1] >= position[3]) return 0;
    *x = position[0];
    *y = position[1];
    g_frame_w = (int)position[2];
    g_frame_h = (int)position[3];
    return 1;
}

static void serve(int client_fd, int ui_fd)
{
    /* Tell the host the guest is ready. */
    uint32_t ready = INPUT_READY_MAGIC;
    if (send_exact(client_fd, &ready, sizeof(ready)) < 0) {
        in_log("send IRDY: %s", strerror(errno));
        return;
    }

    uint32_t keyboard_version = 1;
    uint32_t mouse_version = 0;
    int relative_fd = -1;
    int after_relative = 0;
    uint8_t held[KEY_CNT] = {0};
    while (!g_stop) {
        InputPacket pkt;
        if (recv_exact(client_fd, &pkt, sizeof(pkt)) < 0) break;
        if (pkt.magic != INPUT_MAGIC) {
            in_log("bad magic 0x%08x — desync, closing", pkt.magic);
            break;
        }
        switch (pkt.type) {
        case INPUT_MOUSE_MOVE:
            do_mouse_move(ui_fd, pkt.param1, pkt.param2, after_relative);
            after_relative = 0;
            break;
        case INPUT_MOUSE_BUTTON: do_mouse_button(ui_fd, pkt.param1, pkt.param2); break;
        case INPUT_MOUSE_WHEEL:  do_mouse_wheel(ui_fd, (int32_t)pkt.param1); break;
        case INPUT_KEY:          do_key(ui_fd, pkt.param1, pkt.param2, pkt.param3); break;
        case INPUT_MOUSE_QUERY: {
            if (pkt.param3) break;
            uint32_t x, y;
            get_pointer_position(&x, &y);
            if (pkt.param1 >= INPUT_MOUSE_VERSION && relative_fd < 0)
                relative_fd = uinput_open_relative();
            InputPacket reply = { INPUT_MAGIC, INPUT_MOUSE_REPLY,
                pkt.param1 >= INPUT_MOUSE_VERSION && relative_fd >= 0 ? INPUT_MOUSE_VERSION : 0,
                pkt.param2, 0 };
            if (send_exact(client_fd, &reply, sizeof(reply)) < 0) goto disconnected;
            mouse_version = reply.param1;
            break;
        }
        case INPUT_MOUSE_RELATIVE:
            if (mouse_version == INPUT_MOUSE_VERSION && pkt.param3 == 0) {
                emit(relative_fd, EV_REL, REL_X, (int32_t)pkt.param1);
                emit(relative_fd, EV_REL, REL_Y, (int32_t)pkt.param2);
                emit_syn(relative_fd);
                after_relative = 1;
            }
            break;
        case INPUT_MOUSE_POSITION_QUERY: {
            if (mouse_version != INPUT_MOUSE_VERSION || pkt.param2 || pkt.param3) break;
            InputPacket reply = { INPUT_MAGIC, INPUT_MOUSE_POSITION_REPLY,
                (uint32_t)INT32_MIN, (uint32_t)INT32_MIN, pkt.param1 };
            get_pointer_position(&reply.param1, &reply.param2);
            if (send_exact(client_fd, &reply, sizeof(reply)) < 0) goto disconnected;
            break;
        }
        case INPUT_KEYBOARD_QUERY: {
            if (pkt.param3) break;
            InputPacket reply = { INPUT_MAGIC, INPUT_KEYBOARD_REPLY,
                pkt.param1 >= INPUT_KEYBOARD_VERSION ? INPUT_KEYBOARD_VERSION : 1,
                pkt.param2, 0 };
            if (send_exact(client_fd, &reply, sizeof(reply)) < 0) goto disconnected;
            release_physical_keys(ui_fd, held);
            keyboard_version = reply.param1;
            break;
        }
        case INPUT_KEY_PHYSICAL:
            if (keyboard_version >= INPUT_KEYBOARD_VERSION)
                do_physical_key(ui_fd, pkt.param1, pkt.param2, pkt.param3, held);
            break;
        default: break;
        }
    }
disconnected:
    release_physical_keys(ui_fd, held);
    uinput_close(relative_fd);
}

int main(void)
{
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

    int ui = uinput_open();
    if (ui < 0) return 1;
    in_log("uinput device created");

    int srv = vsock_listen(VSOCK_PORT);
    if (srv < 0) { uinput_close(ui); return 1; }
    in_log("listening on vsock :%u", VSOCK_PORT);

    while (!g_stop) {
        struct sockaddr_vm peer;
        socklen_t plen = sizeof(peer);
        int c = accept(srv, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            if (errno == EINTR) continue;
            in_log("accept: %s", strerror(errno));
            break;
        }
        in_log("client connected (cid=%u)", peer.svm_cid);
        serve(c, ui);
        close(c);
        in_log("client disconnected");
    }

    close(srv);
    uinput_close(ui);
    return 0;
}
