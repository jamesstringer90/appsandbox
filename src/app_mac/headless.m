/*
 * headless.m -- AppSandbox headless daemon (AppSandbox --headless), macOS.
 *
 * Port of src/app_win/headless.c: hosts the core for the process lifetime and
 * serves the SAME Docker-style local HTTP/JSON API over loopback. A discovery
 * file (~/Library/Application Support/AppSandbox/host.json) gives clients the
 * port + bearer token, so asb.py's connect() works unchanged.
 *
 * Platform differences (capability-gated, not interface-changed):
 *   - capabilities: {snapshots:false, templates:false} -> those routes 501.
 *   - "building" is always false (macOS has no host-side disk-build phase;
 *     the whole create is "installing" until the agent stage completes).
 *   - The daemon owns its VMs: VZ VMs live in-process, so daemon exit
 *     terminates them (the same contract the Windows daemon now has).
 *
 * Threading:
 *   - The MAIN thread runs CFRunLoopRun(): it drains the main dispatch queue,
 *     which is where every VZ operation and every core mutation happens
 *     (asb_core_mac marshals via run_on_main; the agent/proxy callbacks are
 *     main-queue). No NSApplication and no window server -- iso-patch-mac
 *     proves VZ runs fine under a bare CFRunLoop, so this daemon works from
 *     a pure SSH session.
 *   - The HTTP accept loop runs on ONE background thread and handles one
 *     request to completion before the next -> all core calls are serialized,
 *     marshalled onto the main queue with dispatch_sync (reads) or
 *     dispatch_async (the create, so an admin-authorization prompt can't
 *     wedge the API). SSE clients get a detached thread each (write-only).
 *
 * Diagnostics go to headless.log next to vms.cfg, and to stderr (the daemon
 * is launched from a terminal).
 */

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>           /* NSApplication run loop (display windows in a GUI session) */
#import <Virtualization/Virtualization.h>
#include <strings.h>               /* strcasecmp */
#import "asb_core_mac.h"
#import "vz_display.h"              /* VzDisplayWindow.userClosed (displayOpen status) */
#import "host_info.h"
#import "vm_dir.h"
#include "headless.h"
#include "asb_types.h"

#include <sys/socket.h>
#include <sys/file.h>
#include <pwd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>

#define ASB_API_VERSION  "v1"
#define DEFAULT_PORT     8787
#define PORT_TRIES       20
#define BODY_MAX         8192
#define REQ_HDR_MAX      16384

/* ---- Logging ---- */

static FILE            *g_log;
static pthread_mutex_t  g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static void hlog(NSString *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSDateFormatter *df = [[NSDateFormatter alloc] init];
    df.dateFormat = @"HH:mm:ss.SSS";
    NSString *line = [NSString stringWithFormat:@"%@  %@",
                      [df stringFromDate:[NSDate date]], msg];
    pthread_mutex_lock(&g_log_mu);
    if (g_log) { fprintf(g_log, "%s\n", line.UTF8String); fflush(g_log); }
    fprintf(stderr, "%s\n", line.UTF8String);
    pthread_mutex_unlock(&g_log_mu);
}

/* ---- Paths ---- */

static NSString *support_dir(void) {   /* .../Application Support/AppSandbox */
    return [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
}

static NSString *discovery_path(void) {
    return [support_dir() stringByAppendingPathComponent:@"host.json"];
}

static void open_log(void) {
    NSString *path = [support_dir() stringByAppendingPathComponent:@"headless.log"];
    g_log = fopen(path.fileSystemRepresentation, "w");
}

static NSString *product_version(void) {
    /* CFBundleShortVersionString is stamped from Directory.Build.props at build
       time; the fallback is a sentinel so a missing stamp is obvious. */
    NSString *v = [NSBundle mainBundle].infoDictionary[@"CFBundleShortVersionString"];
    return v.length ? v : @"0.0.0";
}

/* ---- Single-instance lock (shared with the GUI, see headless.h) ---- */

int asb_instance_lock_acquire(void) {
    NSString *path = [support_dir() stringByAppendingPathComponent:@"instance.lock"];
    int fd = open(path.fileSystemRepresentation, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    /* CLOEXEC so spawned children (QEMU, iso-patch-mac, ssh-keygen, ...) do NOT inherit this fd.
       Otherwise an orphaned child (e.g. a QEMU VM left running after the app quits) keeps the
       open-file-description alive and the flock held -> a relaunch fails "another instance running". */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;   /* held for the process lifetime; OS releases on exit/crash */
}

/* ---- Ownership hand-back (sudo model) ----
 * The daemon runs as root (sudo) but operates on the INVOKING user's
 * AppSandbox tree (vm_dir.m resolves SUDO_USER). Everything root creates
 * there -- vms.cfg, VM dirs/disk images, the downloaded restore image, the
 * ssh keypair, host.json -- must end up owned by the user again, or the GUI
 * and the unprivileged SDK client stop working after the daemon exits. */

static uid_t g_user_uid = 0;
static gid_t g_user_gid = 0;
static dispatch_queue_t g_chown_q;
static int   g_chown_pending = 0;   /* touched only on g_chown_q */

/* Resolve the invoking user's uid/gid from the SAME source vm_dir.m uses to
 * resolve the tree location -- getpwnam(SUDO_USER) -- so the chown target and
 * the directory can never disagree (using $SUDO_UID independently could chown
 * the user's files to a different/zero id). Returns NO if SUDO_USER is set but
 * unresolvable (the daemon must not silently operate on the wrong tree). */
static BOOL resolve_sudo_user(void) {
    const char *u = getenv("SUDO_USER");
    if (!u || !*u) return YES;   /* true root: g_user_uid stays 0 (warned at startup) */
    struct passwd *pw = getpwnam(u);
    if (!pw) return NO;
    g_user_uid = pw->pw_uid;
    g_user_gid = pw->pw_gid;
    return YES;
}

static void chown_to_user(NSString *path) {
    if (g_user_uid && path.length)
        chown(path.fileSystemRepresentation, g_user_uid, g_user_gid);
}

static void fix_tree_ownership_now(void) {
    if (!g_user_uid) return;
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = @"/usr/sbin/chown";
    t.arguments  = @[@"-R",
                     [NSString stringWithFormat:@"%u:%u",
                          (unsigned)g_user_uid, (unsigned)g_user_gid],
                     support_dir()];
    if ([t launchAndReturnError:nil]) [t waitUntilExit];
}

/* Debounced (5s) so chatty events coalesce; chown -R is metadata-only and the
 * tree is a few hundred files, so each pass is cheap. The pending flag is read
 * and written only on g_chown_q (serial), so no cross-thread race. */
static void fix_tree_ownership_soon(void) {
    if (!g_user_uid) return;
    dispatch_async(g_chown_q, ^{
        if (g_chown_pending) return;   /* a pass is already scheduled */
        g_chown_pending = 1;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                       g_chown_q, ^{
            g_chown_pending = 0;
            fix_tree_ownership_now();
        });
    });
}

/* ---- SSE event broadcast (GET /v1/events) -- mirrors headless.c ---- */

#define EV_CAP 256
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ev_cv = PTHREAD_COND_INITIALIZER;
static char           *g_ev_ring[EV_CAP];
static long             g_ev_seq = 0;
static volatile int     g_stop_all = 0;

static void broadcast_event(NSString *json) {
    pthread_mutex_lock(&g_ev_mu);
    free(g_ev_ring[g_ev_seq % EV_CAP]);
    g_ev_ring[g_ev_seq % EV_CAP] = strdup(json.UTF8String);
    g_ev_seq++;
    pthread_cond_broadcast(&g_ev_cv);
    pthread_mutex_unlock(&g_ev_mu);
}

static NSString *json_str(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : @"{}";
}

/* ---- Core event callback ----
 * Fires on the main queue for everything data-bearing. NOTE: AGENT_STATUS's
 * int_value is overloaded (online
 * flag / ssh state / proxy event), so the daemon never interprets it -- GET
 * reads the live core fields instead; SSE forwards the same three event kinds
 * the Windows daemon emits (state/progress/alert). */
static void headless_event_cb(int type, const char *vm_name,
                              int int_value, const char *str_value) {
    NSString *name = vm_name ? @(vm_name) : @"";
    switch (type) {
    case CORE_VM_EVENT_STATE_CHANGED:
        hlog(@"[event] state    : %-20s running=%d", vm_name ?: "", int_value);
        broadcast_event(json_str(@{ @"event": @"vmStateChanged", @"name": name,
                                    @"running": int_value ? @YES : @NO }));
        fix_tree_ownership_soon();
        break;
    case CORE_VM_EVENT_LIST_CHANGED:
        /* create/delete/save just touched the tree -- hand it back to the user */
        fix_tree_ownership_soon();
        break;
    case CORE_VM_EVENT_PROGRESS:
        hlog(@"[event] progress : %-20s %3d%%", vm_name ?: "", int_value);
        broadcast_event(json_str(@{ @"event": @"vmProgress", @"name": name,
                                    @"progress": @(int_value), @"staging": @NO }));
        break;
    case CORE_VM_EVENT_ALERT:
        hlog(@"[event] ALERT    : %s", str_value ?: "");
        broadcast_event(json_str(@{ @"event": @"alert",
                                    @"message": str_value ? @(str_value) : @"" }));
        break;
    case CORE_VM_EVENT_LOG:
        hlog(@"[core] %s", str_value ?: "");
        break;
    case CORE_VM_EVENT_DIAG:
        hlog(@"[diag] %s", str_value ?: "");
        break;
    default:   /* AGENT_STATUS / INSTALL_STATUS / LIST_CHANGED: poll-observable */
        break;
    }
}

/* ---- Token + discovery file ---- */

static char g_token[48];

static void gen_token(void) {
    uint8_t r[20];
    arc4random_buf(r, sizeof(r));
    for (int i = 0; i < 20; i++)
        snprintf(g_token + i * 2, 3, "%02x", r[i]);
    g_token[40] = 0;
}

static void write_discovery(int port) {
    NSDictionary *d = @{
        @"endpoint":   [NSString stringWithFormat:@"http://127.0.0.1:%d", port],
        @"port":       @(port),
        @"token":      @(g_token),
        @"pid":        @(getpid()),
        @"version":    product_version(),
        @"apiVersion": @ASB_API_VERSION,
    };
    NSString *path = discovery_path();
    [json_str(d) writeToFile:path atomically:YES
                    encoding:NSUTF8StringEncoding error:nil];
    chmod(path.fileSystemRepresentation, 0600);   /* token gate = file perms */
    chown_to_user(path);   /* the unprivileged SDK client must be able to read it */
    hlog(@"discovery file written: %@", path);
}

static void delete_discovery(void) {
    [[NSFileManager defaultManager] removeItemAtPath:discovery_path() error:nil];
}

/* ---- Derived state (macOS) ---- */

static const char *derive_state(const AsbVmMac *v) {
    if (!v->disk_built && v->install_progress >= 0)       return "installing";  /* host building the disk */
    if (!v->running)                                      return "stopped";
    if (v->shutting_down)                                 return "stopping";
    if (!v->install_complete)                             return "installing";  /* guest first-boot install
                                                                                   (Windows: until the agent
                                                                                   connects). macOS sets
                                                                                   install_complete at build,
                                                                                   so it never lands here. */
    if (!v->agent_online)                                 return "booting";
    return "online";
}

/* TRUE while the guest OS install/stage is in flight -- the macOS analogue of
   Windows' building_vhdx for the "don't tear the daemon down under it" guard
   (an exit mid-install orphans the iso-patch-mac child + half-built disk). */
static BOOL vm_installing(const AsbVmMac *v) {
    return !v->disk_built && v->install_progress >= 0;
}

/* Cheap per-VM status object -- field-for-field the Windows daemon's
   append_vm_json, plus installStatus (macOS exposes a phase string). */
static NSDictionary *vm_status_dict(const AsbVmMac *v) {
    return @{
        @"name":            @(v->name),
        @"osType":          @(v->os_type),
        @"state":           @(derive_state(v)),
        @"running":         v->running ? @YES : @NO,
        @"agentOnline":     v->agent_online ? @YES : @NO,
        @"installComplete": v->install_complete ? @YES : @NO,
        @"building":        @NO,
        @"progress":        @(v->install_progress < 0 ? 0 : v->install_progress),
        @"installStatus":   @(v->install_status),
        @"sshState":        @((v->ssh_key_deployed && v->ssh_state == 2) ? 4 : v->ssh_state),
        @"sshPort":         @(v->ssh_port),
        @"ramMb":           @(v->ram_mb),
        @"hddGb":           @(v->hdd_gb),
        @"cpuCores":        @(v->cpu_cores),
        @"gpuMode":         @(v->gpu_mode),
        @"networkMode":     @(v->network_mode),
        /* Live window state: nil ref or a user-(X)-closed one both read false,
           mirroring the Windows daemon's is_open. */
        @"displayOpen":     (v->display != nil && !v->display.userClosed) ? @YES : @NO,
    };
}

/* ---- Create-input validation ----
 * Mirrors web/app.js validateVmName/validateUsername/validatePassword exactly
 * as they apply on a macOS host. osType may be "macOS" or "Windows" (anything
 * else is rejected; Windows additionally requires an ISO and disallows
 * templates), plus the numeric rules the Windows daemon enforces, so the API
 * rejects exactly what the UI would. Returns an English error (nil = valid). */
static NSString *validate_create_mac(NSString *name, NSString *os,
                                     NSString *user, BOOL is_template,
                                     int ram_mb, int hdd_gb, int cpu_cores,
                                     int gpu_mode, int net_mode) {
    BOOL is_mac = [os.lowercaseString isEqualToString:@"macos"];
    BOOL is_win = [os.lowercaseString isEqualToString:@"windows"];
    if (!is_mac && !is_win)
        return @"Only macOS and Windows guests are supported on a macOS host.";
    if (is_template)
        return @"Templates are not yet supported for Windows-on-Mac (use a direct ISO install).";

    /* name / hostname (macOS LocalHostName rules, app.js validateVmName) */
    if (!name.length) return @"VM name is required.";
    if (name.length > 63) return @"VM name cannot exceed 63 characters (macOS LocalHostName limit).";
    BOOL all_digits = YES;
    for (NSUInteger i = 0; i < name.length; i++) {
        unichar ch = [name characterAtIndex:i];
        BOOL ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) return @"VM name can only contain letters, digits, and hyphens.";
        if (!(ch >= '0' && ch <= '9')) all_digits = NO;
    }
    if (all_digits) return @"VM name cannot be only digits.";
    if ([name hasPrefix:@"-"] || [name hasSuffix:@"-"])
        return @"VM name cannot start or end with a hyphen.";
    if (asb_mac_vm_find(name.UTF8String))
        return @"A VM with this name already exists.";

    /* username -- app.js applies the Windows-account ruleset on macOS */
    if (!user.length) return @"Username is required.";
    if (user.length > 20) return @"Username cannot exceed 20 characters.";
    BOOL only_dots_ws = YES;
    for (NSUInteger i = 0; i < user.length; i++) {
        unichar ch = [user characterAtIndex:i];
        if ([@"\"\\/[]:;|=,+*?<>" rangeOfString:
                [NSString stringWithCharacters:&ch length:1]].location != NSNotFound)
            return @"Username contains invalid characters.";
        if (!(ch == '.' || ch == ' ' || ch == '\t')) only_dots_ws = NO;
    }
    if (only_dots_ws) return @"Username cannot be only dots or spaces.";
    if ([user hasSuffix:@"."]) return @"Username cannot end with a period.";
    NSArray *reserved = @[@"CON",@"PRN",@"AUX",@"NUL",
        @"COM1",@"COM2",@"COM3",@"COM4",@"COM5",@"COM6",@"COM7",@"COM8",@"COM9",
        @"LPT1",@"LPT2",@"LPT3",@"LPT4",@"LPT5",@"LPT6",@"LPT7",@"LPT8",@"LPT9"];
    if ([reserved containsObject:user.uppercaseString])
        return @"Username is a reserved name.";
    /* Windows guests: the VM name becomes the guest ComputerName; a user named
       like the machine cannot be added to local groups during the unattend pass
       (ERROR_NO_SUCH_MEMBER), so the account ends up group-less and the agent
       never installs. */
    if (is_win && [user caseInsensitiveCompare:name] == NSOrderedSame)
        return @"Username cannot be the same as the VM name.";

    /* numeric ranges (0 = unset -> the core fills a default). The even-RAM
       rule is kept for interface parity with the GUI/Windows daemon. */
    if (ram_mb != 0) {
        if (ram_mb < 512)    return @"RAM must be at least 512 MB.";
        if (ram_mb % 2 != 0) return @"RAM must be 2 MB-aligned (an even number of MB).";
    }
    if (hdd_gb != 0 && hdd_gb < 1)       return @"Disk size must be at least 1 GB.";
    if (cpu_cores != 0 && cpu_cores < 1) return @"CPU cores must be at least 1.";
    if (gpu_mode < 0 || gpu_mode > 2)    return @"gpuMode must be 0 (None), 1 (Default), or 2 (Try all).";
    if (net_mode < 0 || net_mode > 3)    return @"networkMode must be 0 (None), 1 (NAT), 2 (External), or 3 (Internal).";

    return nil;
}

/* ---- Minimal HTTP plumbing (loopback only, Connection: close) ---- */

typedef struct {
    char method[8];
    char path[1024];
    char auth[128];        /* raw Authorization header value */
    char body[BODY_MAX + 1];
} HttpReq;

static int g_listen_fd = -1;

static void send_raw(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void send_response(int fd, int status, const char *reason,
                          const char *ctype, NSString *body) {
    const char *b = body.UTF8String ?: "";
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, reason, ctype, strlen(b));
    send_raw(fd, hdr, (size_t)n);
    send_raw(fd, b, strlen(b));
}

static void send_json(int fd, int status, const char *reason, NSString *body) {
    send_response(fd, status, reason, "application/json", body);
}

static void send_err(int fd, int status, const char *reason,
                     NSString *code, NSString *msg) {
    send_json(fd, status, reason,
              json_str(@{ @"error": @{ @"code": code, @"message": msg } }));
}

/* 202 action result / error -- the macOS analogue of the Windows send_hr.
   BACKEND_* codes map to the same HTTP statuses the Windows daemon produces:
   success and idempotent no-ops -> 202; unknown VM -> 404; bad input -> 400;
   anything else -> 500 (details arrive via the alert event, as on Windows). */
static void send_rc(int fd, const char *action, NSString *name, int rc) {
    if (rc == BACKEND_OK || rc == BACKEND_ERR_ALREADY_RUNNING ||
        rc == BACKEND_ERR_NOT_RUNNING) {
        send_json(fd, 202, "Accepted",
                  json_str(@{ @"ok": @YES, @"accepted": @YES,
                              @"action": @(action), @"name": name }));
    } else if (rc == BACKEND_ERR_NOT_FOUND) {
        send_err(fd, 404, "Not Found", @"not_found", @"no such VM");
    } else if (rc == BACKEND_ERR_INVALID_ARG) {
        send_err(fd, 400, "Bad Request", @"invalid_arg", @"invalid argument");
    } else {
        send_json(fd, 500, "Internal Server Error",
                  json_str(@{ @"ok": @NO, @"action": @(action), @"name": name,
                              @"error": @{ @"rc": @(rc) } }));
    }
}

static void send_501(int fd, NSString *what) {
    send_err(fd, 501, "Not Implemented", @"not_supported",
             [NSString stringWithFormat:@"%@ are not supported on macOS", what]);
}

/* Read one request: headers (<=16K) then Content-Length body (<=8K). */
static int read_request(int fd, HttpReq *r) {
    char buf[REQ_HDR_MAX + 1];
    int pos = 0;
    char *hdr_end = NULL;
    memset(r, 0, sizeof(*r));

    while (pos < REQ_HDR_MAX) {
        ssize_t n = recv(fd, buf + pos, (size_t)(REQ_HDR_MAX - pos), 0);
        if (n <= 0) return -1;
        pos += (int)n;
        buf[pos] = 0;
        if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
    }
    if (!hdr_end) return -1;
    int body_start = (int)(hdr_end - buf) + 4;

    /* request line */
    if (sscanf(buf, "%7s %1023s", r->method, r->path) != 2) return -1;

    /* headers */
    long content_length = 0;
    for (char *line = strstr(buf, "\r\n"); line && line < hdr_end; ) {
        line += 2;
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            content_length = strtol(line + 15, NULL, 10);
        else if (strncasecmp(line, "Authorization:", 14) == 0) {
            const char *v = line + 14;
            while (*v == ' ') v++;
            size_t len = (size_t)(eol - v);
            if (len >= sizeof(r->auth)) len = sizeof(r->auth) - 1;
            memcpy(r->auth, v, len);
            r->auth[len] = 0;
        }
        line = eol;
    }

    if (content_length < 0) content_length = 0;
    if (content_length > BODY_MAX) content_length = BODY_MAX;   /* hard cap */

    int have = pos - body_start;
    if (have > (int)content_length) have = (int)content_length;
    if (have > 0) memcpy(r->body, buf + body_start, (size_t)have);
    while (have < content_length) {
        ssize_t n = recv(fd, r->body + have, (size_t)(content_length - have), 0);
        if (n <= 0) break;
        have += (int)n;
    }
    r->body[have > 0 ? have : 0] = 0;
    return 0;
}

static NSDictionary *parse_body(const HttpReq *r) {
    if (!r->body[0]) return @{};
    NSData *d = [NSData dataWithBytes:r->body length:strlen(r->body)];
    id obj = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
    return [obj isKindOfClass:[NSDictionary class]] ? obj : @{};
}

static int auth_ok(const HttpReq *r) {
    char expect[64];
    snprintf(expect, sizeof(expect), "Bearer %s", g_token);
    return strcmp(r->auth, expect) == 0;   /* exact, case-sensitive */
}

/* Run a block synchronously on the main queue (where the core lives). */
static void on_main(dispatch_block_t block) {
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

/* ---- SSE client thread (write-only; never touches the core) ---- */

static void *sse_thread(void *param) {
    int fd = (int)(intptr_t)param;
    pthread_detach(pthread_self());

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "retry: 3000\n\n";
    send_raw(fd, hdr, strlen(hdr));

    pthread_mutex_lock(&g_ev_mu);
    long my_seq = g_ev_seq;   /* only stream events from connect onward */
    pthread_mutex_unlock(&g_ev_mu);

    int alive = 1;
    while (alive && !g_stop_all) {
        char *frames[64];
        int nframes = 0;

        pthread_mutex_lock(&g_ev_mu);
        if (my_seq == g_ev_seq && !g_stop_all) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 15;   /* timeout -> heartbeat */
            pthread_cond_timedwait(&g_ev_cv, &g_ev_mu, &ts);
        }
        if (g_ev_seq - my_seq > EV_CAP) my_seq = g_ev_seq - EV_CAP;  /* dropped */
        while (my_seq < g_ev_seq && nframes < 64) {
            char *s = g_ev_ring[my_seq % EV_CAP];
            frames[nframes++] = s ? strdup(s) : NULL;
            my_seq++;
        }
        pthread_mutex_unlock(&g_ev_mu);

        if (nframes == 0) {
            if (send(fd, ": ping\n\n", 8, 0) != 8) alive = 0;   /* heartbeat */
        } else {
            for (int i = 0; i < nframes && alive; i++) {
                if (!frames[i]) continue;
                char frame[2048];
                int n = snprintf(frame, sizeof(frame), "data: %s\n\n", frames[i]);
                if (n > 0 && n < (int)sizeof(frame)) {
                    ssize_t w = send(fd, frame, (size_t)n, 0);
                    if (w != n) alive = 0;
                }
            }
        }
        for (int i = 0; i < nframes; i++) free(frames[i]);
    }
    close(fd);
    return NULL;
}

/* ---- Clean exit ---- */

static int g_lock_fd = -1;

static void cleanup_and_exit(int code) {
    /* Reachable from the SIGINT/SIGTERM dispatch sources and the /v1/shutdown
       block (all on the main queue). Guard against a second pass (e.g. SIGTERM
       arriving while the first teardown is mid-flight). */
    static int done = 0;
    if (done) return;
    done = 1;
    g_stop_all = 1;
    pthread_cond_broadcast(&g_ev_cv);
    hlog(@"stopping: deleting discovery, cleaning up (VMs terminate with the daemon).");
    delete_discovery();
    if (g_listen_fd >= 0) close(g_listen_fd);
    /* Orderly teardown of agent/proxy/clipboard threads; the VZ VMs themselves
       are in-process and terminate with us -- the daemon owns its VMs. */
    asb_mac_cleanup();
    /* Final synchronous hand-back: everything root touched in the user's
       AppSandbox tree is the user's again before we exit. */
    fix_tree_ownership_now();
    hlog(@"cleaned up. exit %d.", code);
    if (g_log) { fclose(g_log); g_log = NULL; }
    if (g_lock_fd >= 0) close(g_lock_fd);
    exit(code);
}

/* ---- Request dispatch. Returns 1 if the daemon should stop. ---- */

static int handle_request(int fd, HttpReq *r) {
    NSString *path = @(r->path);
    BOOL isGET    = strcmp(r->method, "GET") == 0;
    BOOL isPOST   = strcmp(r->method, "POST") == 0;
    BOOL isPUT    = strcmp(r->method, "PUT") == 0;
    /* (no bare DELETE routes on macOS -- snapshots/templates 501 before the
       method matters, and VM delete is POST /vms/{name}/delete.) */

    /* /v1/version is open; everything else requires the token. */
    if (isGET && [path isEqualToString:@"/v1/version"]) {
        send_json(fd, 200, "OK", json_str(@{
            @"product": @"AppSandbox", @"version": product_version(),
            @"apiVersion": @ASB_API_VERSION, @"hostOs": @"macOS",
            @"capabilities": @{ @"snapshots": @NO, @"templates": @NO } }));
        return 0;
    }
    if (!auth_ok(r)) {
        send_err(fd, 401, "Unauthorized", @"unauthorized",
                 @"missing or bad bearer token");
        return 0;
    }

    /* SSE event stream -- handed to a dedicated thread. */
    if (isGET && [path isEqualToString:@"/v1/events"]) {
        pthread_t t;
        if (pthread_create(&t, NULL, sse_thread, (void *)(intptr_t)fd) == 0)
            return 2;   /* socket ownership moved to the SSE thread */
        send_err(fd, 500, "Internal Server Error", @"thread", @"spawn failed");
        return 0;
    }

    if (isGET && [path isEqualToString:@"/v1/host"]) {
        __block NSDictionary *info;
        on_main(^{
            int vmCores = 0, vmRamMb = 0, vmHddGb = 0;
            int count = asb_mac_vm_count();
            for (int i = 0; i < count; i++) {
                AsbVmMac *v = asb_mac_vm_get(i);
                if (!v) continue;
                if (v->running) { vmCores += v->cpu_cores; vmRamMb += v->ram_mb; }
                vmHddGb += v->hdd_gb;
            }
            info = @{ @"hostCores": @([HostInfo hostCores]),
                      @"hostRamMb": @([HostInfo hostRamMb]),
                      @"freeGb":    @([HostInfo freeGb]),
                      @"vmCores":   @(vmCores), @"vmRamMb": @(vmRamMb),
                      @"vmHddGb":   @(vmHddGb) };
        });
        send_json(fd, 200, "OK", json_str(info));
        return 0;
    }

    /* ---- Templates: not a macOS feature (capabilities.templates=false) ---- */
    if ([path hasPrefix:@"/v1/templates"]) {
        send_501(fd, @"templates");
        return 0;
    }

    /* ---- /v1/vms (collection) ---- */
    if ([path isEqualToString:@"/v1/vms"]) {
        if (isGET) {
            __block NSMutableArray *vms = [NSMutableArray array];
            on_main(^{
                int count = asb_mac_vm_count();
                for (int i = 0; i < count; i++) {
                    AsbVmMac *v = asb_mac_vm_get(i);
                    if (v) [vms addObject:vm_status_dict(v)];
                }
            });
            send_json(fd, 200, "OK", json_str(@{ @"vms": vms }));
            return 0;
        }
        if (isPOST) {
            NSDictionary *b = parse_body(r);
            NSString *name = [[b[@"name"] description] stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceAndNewlineCharacterSet]];
            if (!b[@"name"]) name = @"";
            NSString *os   = b[@"osType"] ? [b[@"osType"] description] : @"macOS";
            NSString *img  = b[@"imagePath"] ? [b[@"imagePath"] description] : @"";
            NSString *user = b[@"adminUser"] ? [[b[@"adminUser"] description]
                                stringByTrimmingCharactersInSet:
                                [NSCharacterSet whitespaceAndNewlineCharacterSet]] : @"";
            NSString *pass = b[@"adminPass"] ? [b[@"adminPass"] description] : @"";
            int ram = [b[@"ramMb"] intValue], hdd = [b[@"hddGb"] intValue];
            int cpu = [b[@"cpuCores"] intValue], gpu = [b[@"gpuMode"] intValue];
            int net = [b[@"networkMode"] intValue];
            BOOL sshEnabled = [b[@"sshEnabled"] boolValue];
            BOOL sshDeploy  = [b[@"sshDeployKey"] boolValue];
            BOOL testMode   = [b[@"testMode"] boolValue];
            BOOL isTemplate = [b[@"isTemplate"] boolValue];

            if (sshDeploy && !sshEnabled) {
                send_err(fd, 400, "Bad Request", @"invalid_arg",
                         @"sshDeployKey requires sshEnabled");
                return 0;
            }
            __block NSString *verr;
            on_main(^{
                verr = validate_create_mac(name, os, user, isTemplate,
                                           ram, hdd, cpu, gpu, net);
            });
            if (verr) {
                send_err(fd, 400, "Bad Request", @"invalid_arg", verr);
                return 0;
            }
            /* Windows-on-Mac is a from-scratch ISO install: the ISO is mandatory. */
            if ([os.lowercaseString isEqualToString:@"windows"] && img.length == 0) {
                send_err(fd, 400, "Bad Request", @"invalid_arg",
                         @"A Windows ISO (imagePath) is required to create a Windows VM.");
                return 0;
            }

            /* Async create (202 then events/status), like Windows -- and the
               admin-authorization prompt inside create must not block the
               request loop, so this is dispatch_async, not sync. A sync
               failure (auth denied, dir create, ...) surfaces as an alert
               event + the VM never appearing, exactly the async-create
               contract clients already handle. */
            dispatch_async(dispatch_get_main_queue(), ^{
                int rc = asb_mac_vm_create(name.UTF8String, os.UTF8String,
                                           ram, hdd, cpu, gpu, net,
                                           img.length ? img.UTF8String : NULL,
                                           user.UTF8String, pass.UTF8String,
                                           sshEnabled, sshDeploy, testMode);
                if (rc != BACKEND_OK)
                    hlog(@"create '%@' failed rc=%d (alert event posted)", name, rc);
            });
            send_json(fd, 202, "Accepted",
                      json_str(@{ @"ok": @YES, @"accepted": @YES,
                                  @"action": @"createVm", @"name": name }));
            return 0;
        }
    }

    /* ---- /v1/vms/{name}[/sub] ---- */
    if ([path hasPrefix:@"/v1/vms/"]) {
        NSString *rest = [path substringFromIndex:8];
        NSRange slash = [rest rangeOfString:@"/"];
        NSString *name = (slash.location == NSNotFound)
                             ? rest : [rest substringToIndex:slash.location];
        NSString *sub  = (slash.location == NSNotFound)
                             ? nil  : [rest substringFromIndex:slash.location + 1];
        if (name.length == 0 || name.length > 255) {
            send_err(fd, 400, "Bad Request", @"invalid_arg", @"bad VM name");
            return 0;
        }

        __block BOOL exists = NO;
        on_main(^{ exists = asb_mac_vm_find(name.UTF8String) != NULL; });
        if (!exists) {
            send_err(fd, 404, "Not Found", @"not_found", @"no such VM");
            return 0;
        }

        if (sub == nil) {
            if (isGET) {
                __block NSDictionary *st = nil;
                on_main(^{
                    AsbVmMac *v = asb_mac_vm_find(name.UTF8String);
                    if (v) st = vm_status_dict(v);
                });
                if (!st) send_err(fd, 404, "Not Found", @"not_found", @"no such VM");
                else     send_json(fd, 200, "OK", json_str(st));
                return 0;
            }
            if (isPUT) {   /* edit -- VM must be stopped (and not installing) */
                NSDictionary *b = parse_body(r);
                __block int conflict = 0;
                on_main(^{
                    AsbVmMac *v = asb_mac_vm_find(name.UTF8String);
                    if (v && vm_installing(v)) conflict = 2;
                    else if (v && v->running)  conflict = 1;
                });
                if (conflict == 2) {
                    send_err(fd, 409, "Conflict", @"vm_building",
                             @"cannot edit a VM while it is installing; wait for the install to finish");
                    return 0;
                }
                if (conflict == 1) {
                    send_err(fd, 409, "Conflict", @"vm_running",
                             @"cannot edit a running VM; stop it first");
                    return 0;
                }
                if (b[@"ramMb"]) {
                    int iv = [b[@"ramMb"] intValue];
                    if (iv < 512 || iv % 2 != 0) {
                        send_err(fd, 400, "Bad Request", @"invalid_arg",
                                 @"RAM must be 2 MB-aligned and at least 512 MB");
                        return 0;
                    }
                }
                if (b[@"cpuCores"] && [b[@"cpuCores"] intValue] < 1) {
                    send_err(fd, 400, "Bad Request", @"invalid_arg",
                             @"CPU cores must be at least 1");
                    return 0;
                }
                if (b[@"gpuMode"]) {
                    int iv = [b[@"gpuMode"] intValue];
                    if (iv < 0 || iv > 2) {
                        send_err(fd, 400, "Bad Request", @"invalid_arg",
                                 @"gpuMode must be 0 (None), 1 (Default), or 2 (Try all)");
                        return 0;
                    }
                }
                if (b[@"networkMode"]) {
                    int iv = [b[@"networkMode"] intValue];
                    if (iv < 0 || iv > 3) {
                        send_err(fd, 400, "Bad Request", @"invalid_arg",
                                 @"networkMode must be 0 (None), 1 (NAT), 2 (External), or 3 (Internal)");
                        return 0;
                    }
                }
                __block int rc = BACKEND_OK;
                __block NSDictionary *st = nil;
                on_main(^{
                    for (NSString *field in @[@"ramMb", @"cpuCores", @"gpuMode", @"networkMode"]) {
                        if (!b[field]) continue;
                        NSString *val = [NSString stringWithFormat:@"%d", [b[field] intValue]];
                        int frc = asb_mac_vm_edit(name.UTF8String, field.UTF8String, val.UTF8String);
                        if (frc != BACKEND_OK) rc = frc;
                    }
                    asb_mac_save();
                    AsbVmMac *v = asb_mac_vm_find(name.UTF8String);
                    if (v) st = vm_status_dict(v);
                });
                if (rc == BACKEND_OK && st) send_json(fd, 200, "OK", json_str(st));
                else                        send_rc(fd, "editVm", name, rc);
                return 0;
            }
            send_err(fd, 405, "Method Not Allowed", @"method", @"unsupported method");
            return 0;
        }

        /* sub-routes */
        if (isPOST && [sub isEqualToString:@"start"]) {
            /* snapIndex/branchIndex bodies are a snapshot feature -> ignored
               (capabilities.snapshots=false; the GUI has no snapshot UI on
               macOS either). A plain start boots the current disk. */
            __block int rc;
            on_main(^{ rc = asb_mac_vm_start(name.UTF8String); });
            send_rc(fd, "start", name, rc);
            return 0;
        }
        if (isPOST && [sub isEqualToString:@"shutdown"]) {
            __block int rc;
            on_main(^{ rc = asb_mac_vm_stop(name.UTF8String, 0); });
            send_rc(fd, "shutdown", name, rc);
            return 0;
        }
        if (isPOST && [sub isEqualToString:@"stop"]) {
            __block int rc;
            on_main(^{ rc = asb_mac_vm_stop(name.UTF8String, 1); });
            send_rc(fd, "stop", name, rc);
            return 0;
        }
        if (isPOST && [sub isEqualToString:@"delete"]) {
            __block int conflict = 0;
            __block int rc = BACKEND_OK;
            on_main(^{
                AsbVmMac *v = asb_mac_vm_find(name.UTF8String);
                if (v && vm_installing(v)) conflict = 1;   /* iso-patch-mac owns the disk */
                else rc = asb_mac_vm_delete(name.UTF8String);
            });
            if (conflict) {
                send_err(fd, 409, "Conflict", @"vm_building",
                         @"cannot delete a VM while it is installing; "
                         @"wait for the install to finish, then delete");
                return 0;
            }
            send_rc(fd, "delete", name, rc);
            return 0;
        }

        if (isGET && [sub isEqualToString:@"sshInfo"]) {
            __block NSDictionary *info = nil;
            on_main(^{
                AsbVmMac *v = asb_mac_vm_find(name.UTF8String);
                if (!v) return;
                info = @{ @"host": @"127.0.0.1",
                          @"port": @(v->ssh_port),
                          @"user": @(v->admin_user[0] ? v->admin_user : "user"),
                          @"sshState": @((v->ssh_key_deployed && v->ssh_state == 2) ? 4 : v->ssh_state),
                          @"enabled": v->ssh_enabled ? @YES : @NO,
                          @"keyDeployed": v->ssh_key_deployed ? @YES : @NO };
            });
            if (!info) send_err(fd, 404, "Not Found", @"not_found", @"no such VM");
            else       send_json(fd, 200, "OK", json_str(info));
            return 0;
        }

        if ([sub isEqualToString:@"display"]) {
            BOOL isDELETE = strcmp(r->method, "DELETE") == 0;
            if (isPOST) {   /* open (or focus) the display window */
                __block int rc = BACKEND_OK;
                on_main(^{ rc = asb_mac_open_display(name.UTF8String); });
                if (rc == BACKEND_OK)
                    send_json(fd, 200, "OK",
                              json_str(@{ @"ok": @YES, @"displayOpen": @YES }));
                else if (rc == BACKEND_ERR_NO_DISPLAY)
                    send_err(fd, 409, "Conflict", @"no_display",
                             @"no local interactive desktop is available to show the "
                             @"window (the daemon is in a non-interactive/service session)");
                else if (rc == BACKEND_ERR_NOT_RUNNING)
                    send_err(fd, 409, "Conflict", @"not_running",
                             @"VM is not running; start it before opening the display");
                else if (rc == BACKEND_ERR_NOT_READY)
                    send_err(fd, 409, "Conflict", @"display_not_ready",
                             @"the VM's guest agent is not online yet; retry shortly");
                else if (rc == BACKEND_ERR_NOT_FOUND)
                    send_err(fd, 404, "Not Found", @"not_found", @"no such VM");
                else
                    send_json(fd, 500, "Internal Server Error",
                              json_str(@{ @"ok": @NO, @"error": @{ @"rc": @(rc) } }));
                return 0;
            }
            if (isDELETE) {   /* close the window (no-op if already closed) */
                on_main(^{ asb_mac_close_display(name.UTF8String); });
                send_json(fd, 200, "OK",
                          json_str(@{ @"ok": @YES, @"displayOpen": @NO }));
                return 0;
            }
            if (isGET) {
                /* Pollable + passive: reads flags, opens no window, touches no frame channel.
                   A macOS guest binds the in-process framebuffer (ready == running && agent_online);
                   a Windows guest streams VDD frames over ivshmem and is ready only once its IDD
                   driver is up (idd_ready) -- matching the open precondition (asb_mac_open_display). */
                __block BOOL open = NO, ready = NO;
                on_main(^{
                    AsbVmMac *v = asb_mac_vm_find(name.UTF8String);
                    if (!v) return;
                    BOOL is_win = (strcasecmp(v->os_type, "Windows") == 0);
                    open  = (v->display != nil && !v->display.userClosed);
                    ready = open || (v->running && v->agent_online && (!is_win || v->idd_ready));
                });
                send_json(fd, 200, "OK",
                          json_str(@{ @"open": open ? @YES : @NO,
                                      @"ready": ready ? @YES : @NO }));
                return 0;
            }
            send_err(fd, 405, "Method Not Allowed", @"method",
                     @"use GET to poll state, POST to open the display, DELETE to close it");
            return 0;
        }

        if ([sub hasPrefix:@"snapshots"]) {
            send_501(fd, @"snapshots");
            return 0;
        }

        send_err(fd, 404, "Not Found", @"not_found", @"no such route");
        return 0;
    }

    /* POST /v1/shutdown -> clean daemon stop. The daemon owns its VMs (VZ is
       in-process), so exit terminates them -- refuse while any VM is running
       or mid-install unless the caller passes {"force":true}. */
    if (isPOST && [path isEqualToString:@"/v1/shutdown"]) {
        BOOL force = [parse_body(r)[@"force"] boolValue];
        __block NSMutableArray *active = [NSMutableArray array];
        on_main(^{
            int count = asb_mac_vm_count();
            for (int i = 0; i < count; i++) {
                AsbVmMac *v = asb_mac_vm_get(i);
                if (v && (v->running || vm_installing(v)))
                    [active addObject:@(v->name)];
            }
        });
        if (active.count && !force) {
            send_json(fd, 409, "Conflict", json_str(@{ @"error": @{
                @"code": @"vms_active",
                @"message": @"VMs are running or still installing; stop/finish "
                            @"them first or POST {\"force\":true} to terminate "
                            @"them on shutdown.",
                @"active": active } }));
            return 0;
        }
        send_json(fd, 200, "OK", json_str(@{ @"ok": @YES, @"stopping": @YES }));
        return 1;
    }

    send_err(fd, 404, "Not Found", @"not_found", @"no such route");
    return 0;
}

/* ---- HTTP server thread: serial accept->handle loop (= the one command
   thread; one request runs to completion before the next). ---- */

static void *server_thread(void *param) {
    (void)param;
    while (!g_stop_all) {
        int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;   /* listener closed -> shutting down */
        }
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        HttpReq req;
        if (read_request(fd, &req) != 0) { close(fd); continue; }

        int rc;
        @autoreleasepool { rc = handle_request(fd, &req); }
        if (rc != 2) close(fd);          /* 2 = SSE thread owns the socket */
        if (rc == 1) {                   /* POST /v1/shutdown accepted */
            dispatch_async(dispatch_get_main_queue(), ^{ cleanup_and_exit(0); });
            break;
        }
    }
    return NULL;
}

static int http_start(int *out_port) {
    for (int port = DEFAULT_PORT; port < DEFAULT_PORT + PORT_TRIES; port++) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) return 0;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback only */
        a.sin_port = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
            listen(fd, 16) == 0) {
            g_listen_fd = fd;
            *out_port = port;
            hlog(@"listening on http://127.0.0.1:%d/", port);
            return 1;
        }
        close(fd);
        hlog(@"port %d unavailable; trying next", port);
    }
    hlog(@"FATAL: no free port in %d..%d.", DEFAULT_PORT, DEFAULT_PORT + PORT_TRIES - 1);
    return 0;
}

/* ---- Entry ---- */

int run_headless(int argc, const char *argv[]) {
    (void)argc; (void)argv;

    /* ---- Fail fast (mirrors the Windows daemon's startup checks, s5.2a):
       every prerequisite is verified at RUNTIME BEFORE anything binds or
       initializes, with a clear reason on stderr -- the daemon never
       half-starts. ---- */

    /* (1) Minimum macOS -- the runtime analogue of Windows' os_supported. The
       required version is the bundle's own LSMinimumSystemVersion (the single
       source of truth), so this never drifts from the build's deployment
       target. A direct --headless launch bypasses the LaunchServices gate that
       protects the GUI, so we must check it ourselves. */
    {
        NSString *minStr = [NSBundle mainBundle].infoDictionary[@"LSMinimumSystemVersion"];
        if (!minStr.length) minStr = @"26.0";
        NSArray<NSString *> *p = [minStr componentsSeparatedByString:@"."];
        NSOperatingSystemVersion minV = {0};
        if (p.count > 0) minV.majorVersion = [p[0] integerValue];
        if (p.count > 1) minV.minorVersion = [p[1] integerValue];
        if (p.count > 2) minV.patchVersion = [p[2] integerValue];
        if (![[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:minV]) {
            fprintf(stderr, "AppSandbox --headless: unsupported macOS -- detected %s, "
                    "requires %s or newer.\n",
                    [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String,
                    minStr.UTF8String);
            return 3;
        }
    }

    /* (2) Virtualization capability -- the runtime analogue of Windows'
       VirtualMachinePlatform check. False on Intel Macs (no Apple-silicon VM
       support) or where the hypervisor/entitlement is unavailable. This also
       supersedes the old compile-time __arm64__ guard (an arm64 binary can't
       even launch on Intel, so the meaningful gate is the capability, not the
       arch). */
    if (![VZVirtualMachine isSupported]) {
        fprintf(stderr, "AppSandbox --headless: Virtualization is not supported on "
                "this Mac (Apple silicon + the Hypervisor entitlement are required).\n");
        return 3;
    }

    /* (3) Elevation, up front -- like Windows' is_elevated check. Root makes the
       daemon fully NON-INTERACTIVE: the privileged install/stage step runs
       directly (no admin password prompt, which would be unscriptable).
       vm_dir.m resolves SUDO_USER so the daemon still operates on the
       invoking user's VM registry + cached restore image, and the ownership
       hand-back below returns every artifact to that user. */
    if (geteuid() != 0) {
        fprintf(stderr,
            "AppSandbox --headless must be run elevated (VM creation stages "
            "the guest disk as root,\nand prompting for a password would not "
            "be scriptable). Run it with sudo:\n"
            "    sudo \"%s\" --headless\n",
            [NSProcessInfo processInfo].arguments.firstObject.UTF8String
                ?: "AppSandbox.app/Contents/MacOS/AppSandbox");
        return 3;
    }
    /* Resolve the invoking user from SUDO_USER (same source vm_dir.m uses for
       the tree). If SUDO_USER is set but can't be resolved, abort rather than
       silently operate on root's registry with the user's files left orphaned. */
    if (!resolve_sudo_user()) {
        fprintf(stderr, "AppSandbox --headless: cannot resolve SUDO_USER '%s' "
                "-- aborting (would operate on the wrong VM registry).\n",
                getenv("SUDO_USER") ?: "");
        return 3;
    }
    if (!g_user_uid)
        fprintf(stderr,
            "WARNING: no SUDO_USER -- running as true root, so the VM registry "
            "is /var/root/Library/Application Support/AppSandbox.\n");

    /* Lock FIRST, log file after: open_log truncates headless.log, so a
       losing second instance must never touch it (it would wipe the running
       daemon's log mid-write). Pre-lock messages go to stderr via hlog. */
    g_lock_fd = asb_instance_lock_acquire();
    if (g_lock_fd < 0) {
        hlog(@"Another AppSandbox instance (GUI or daemon) is already running. Exiting.");
        return 2;
    }
    g_chown_q = dispatch_queue_create("com.appsandbox.headless.chown",
                                      DISPATCH_QUEUE_SERIAL);
    chown_to_user([support_dir() stringByAppendingPathComponent:@"instance.lock"]);
    open_log();
    chown_to_user([support_dir() stringByAppendingPathComponent:@"headless.log"]);
    hlog(@"=== AppSandbox --headless starting (pid %d, sudo user %u) ===",
         getpid(), (unsigned)g_user_uid);
    hlog(@"single-instance lock acquired.");

    /* Headless BEFORE init/any start: gates the display window, clipboard,
       and VM audio devices (see asb_mac_set_headless). */
    asb_mac_set_headless(YES);
    asb_mac_set_event_cb(headless_event_cb);
    asb_mac_init();
    hlog(@"core initialized (%d VMs).", asb_mac_vm_count());

    gen_token();
    int port = 0;
    if (!http_start(&port)) { asb_mac_cleanup(); return 1; }
    write_discovery(port);

    /* SIGINT/SIGTERM -> clean exit (delete discovery, terminate VMs). */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    dispatch_source_t s_int = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s_int, ^{ cleanup_and_exit(0); });
    dispatch_resume(s_int);
    dispatch_source_t s_term = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s_term, ^{ cleanup_and_exit(0); });
    dispatch_resume(s_term);

    pthread_t server;
    if (pthread_create(&server, NULL, server_thread, NULL) != 0) {
        hlog(@"FATAL: server thread failed.");
        delete_discovery();
        asb_mac_cleanup();
        return 1;
    }

    /* Main run loop. Both [NSApp run] and CFRunLoopRun drain the MAIN DISPATCH
       QUEUE -- where every VZ op, agent callback, and core mutation runs -- so
       the daemon behaves identically either way (and the SIGINT/SIGTERM dispatch
       sources still fire). We use AppKit's loop ONLY in a console GUI session,
       because the display window (a VZVirtualMachineView in an NSWindow) needs the
       window server; with no session we keep the bare CFRunLoop so the daemon
       still runs over SSH / launchd (display-open is then refused by the same
       A-gate). cleanup_and_exit calls exit() directly, so neither loop needs an
       explicit stop. iso-patch-mac proves VZ runs fine under a bare CFRunLoop. */
    if (asb_mac_have_gui_session()) {
        hlog(@"entering AppKit run loop (console GUI session -- display windows available).");
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp run];
    } else {
        hlog(@"entering bare CFRunLoop (no GUI session -- display unavailable).");
        CFRunLoopRun();
    }

    /* Not normally reached (exit paths call cleanup_and_exit). */
    cleanup_and_exit(0);
    return 0;
}
