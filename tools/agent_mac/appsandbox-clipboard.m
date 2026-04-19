/*
 * appsandbox-clipboard -- Guest-side clipboard sync helper.
 *
 * Runs as a LaunchAgent in the active user's GUI session (required for
 * NSPasteboard access). Listens on AF_VSOCK port 5 for the host's
 * VmClipboardMac to connect, then relays pasteboard changes in both
 * directions using a length-prefixed binary protocol identical in shape
 * to the Windows agent's :0005 clipboard channel (tools/agent/
 * appsandbox-clipboard.c) — FORMAT_LIST, DATA_REQ, DATA_RESP, FILE_DATA.
 *
 * Supported types:
 *   public.utf8-plain-text  (NSPasteboardTypeString)
 *   public.rtf              (NSPasteboardTypeRTF)
 *   public.html             (NSPasteboardTypeHTML)
 *   public.png / public.tiff
 *   public.file-url         (file URL list + recursive byte transfer)
 *
 * Echo suppression: after we write the pasteboard with data received from
 * the host, we record the resulting changeCount in g_suppress_change and
 * skip the next one-shot host-push.
 */

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import "clipboard_xpc.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>

/* ---- Wire protocol ---- */

#define CLIP_MAGIC                  0x41434C50u  /* 'ACLP' */

/* V1 (legacy) — eager host→guest copy. */
#define CLIP_MSG_FORMAT_LIST        1u
#define CLIP_MSG_FORMAT_DATA_REQ    2u
#define CLIP_MSG_FORMAT_DATA_RESP   3u
#define CLIP_MSG_FILE_DATA          4u

/* V2 — lazy deferred-rendering. seq_id is the first 4 bytes of the body
 * (big-endian) followed by the V1-equivalent payload. Requests and
 * responses correlate via seq_id so providers running on arbitrary
 * pasteboard-destination threads can each wait for their own response. */
#define CLIP_MSG_FORMAT_LIST_V2      5u
#define CLIP_MSG_FORMAT_DATA_REQ_V2  6u
#define CLIP_MSG_FORMAT_DATA_RESP_V2 7u
#define CLIP_MSG_FILE_DATA_V2        8u
#define CLIP_MSG_FORMAT_DATA_CHUNK   9u   /* streaming body for RESP_V2 */
#define CLIP_MSG_FORMAT_DATA_END     10u  /* terminates chunk stream */

#define CLIP_MAX_PAYLOAD            (256u * 1024u * 1024u)
#define CLIP_FILE_CHUNK             (1u * 1024u * 1024u)
#define CLIP_LARGE_THRESHOLD        (1u * 1024u * 1024u)

/* Darwin's AF_VSOCK bind requires root for any port, and NSPasteboard
 * requires a user GUI session. The root `appsandbox-agent` LaunchDaemon
 * binds vsock :5 and hands each accepted connection to us via XPC (see
 * clipboard_xpc.h). We speak the pasteboard protocol directly on the
 * received fd — no proxying. */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t msg_type;
    uint32_t format_len;
    uint32_t data_size;
} ClipHeader;

typedef struct __attribute__((packed)) {
    uint32_t path_len;
    uint64_t file_size;
    uint32_t is_directory;
} ClipFileInfo;

/* ---- Globals ---- */

static int g_client_sock = -1;
static volatile sig_atomic_t g_stop = 0;
static NSInteger g_last_seen_change = -1;
static _Atomic NSInteger g_suppress_change = -1;
static NSString *g_cache_base;   /* ~/Library/Caches/com.appsandbox.clipboard */

/* ---- V2 pending-request dispatcher ---------------------------------
 * Each outbound V2 DATA_REQ is tagged with a monotonically-increasing
 * seq_id and registered in g_pending. The receive loop parses inbound
 * DATA_RESP_V2 / FILE_DATA_V2 / FORMAT_DATA_CHUNK / FORMAT_DATA_END and
 * routes the body to the matching request by seq_id. The originating
 * thread (e.g. a pasteboard data provider callback on a random app
 * thread) blocks on the request's condition until its body completes.
 *
 * Everything is guarded by the NSCondition lock on the request itself,
 * plus the g_pending_lock on the table. */
@interface AsbClipPending : NSObject
@property (atomic, assign) uint32_t seqId;
@property (nonatomic, strong) NSCondition *cond;
@property (nonatomic, strong) NSMutableData *body;     /* non-file: accumulates chunks */
@property (nonatomic, copy)   NSString *fileDestPath;  /* single file: target write path */
@property (nonatomic, assign) int       fileFd;        /* open dest fd or -1 */
@property (nonatomic, assign) uint64_t  fileBytesWritten;
@property (nonatomic, assign) uint64_t  fileSize;
/* For multi-file (file-url) transfers: destination cache directory, a
 * countdown of expected file messages, and the materialised top-level
 * URLs for the pasteboard to return. */
@property (nonatomic, copy)   NSString *fileTreeDir;
@property (nonatomic, assign) int32_t   fileCountExpected;
@property (nonatomic, strong) NSMutableArray<NSURL *> *fileTreeUrls;
@property (nonatomic, assign) BOOL      completed;
@property (nonatomic, assign) BOOL      failed;
@property (nonatomic, copy)   NSString *failureReason;
@end

@implementation AsbClipPending
- (instancetype)init {
    if ((self = [super init])) {
        _cond = [NSCondition new];
        _body = [NSMutableData data];
        _fileFd = -1;
    }
    return self;
}
@end

static NSMutableDictionary<NSNumber *, AsbClipPending *> *g_pending;
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t g_next_seq = 1;

static uint32_t alloc_seq(void) {
    uint32_t s = atomic_fetch_add(&g_next_seq, 1);
    if (s == 0) s = atomic_fetch_add(&g_next_seq, 1);  /* never hand out 0 */
    return s;
}

static void pending_register(AsbClipPending *p) {
    pthread_mutex_lock(&g_pending_lock);
    if (!g_pending) g_pending = [NSMutableDictionary dictionary];
    g_pending[@(p.seqId)] = p;
    pthread_mutex_unlock(&g_pending_lock);
}

static AsbClipPending *pending_lookup(uint32_t seq) {
    pthread_mutex_lock(&g_pending_lock);
    AsbClipPending *p = g_pending[@(seq)];
    pthread_mutex_unlock(&g_pending_lock);
    return p;
}

static void pending_remove(uint32_t seq) {
    pthread_mutex_lock(&g_pending_lock);
    [g_pending removeObjectForKey:@(seq)];
    pthread_mutex_unlock(&g_pending_lock);
}

/* Fail every in-flight request — used when the vsock drops. */
static void pending_fail_all(NSString *reason) {
    pthread_mutex_lock(&g_pending_lock);
    NSArray *all = g_pending.allValues;
    [g_pending removeAllObjects];
    pthread_mutex_unlock(&g_pending_lock);
    for (AsbClipPending *p in all) {
        [p.cond lock];
        p.failed = YES;
        p.failureReason = reason;
        p.completed = YES;
        [p.cond broadcast];
        [p.cond unlock];
    }
}

/* ---- Logging ---- */

#define LOG_PATH "/tmp/appsandbox-clipboard.log"

static void clip_log(NSString *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(f, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap; va_start(ap, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    fprintf(f, "%s\n", msg.UTF8String);
    fclose(f);
}

/* ---- Socket I/O ---- */

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 0;
        p += r; n -= (size_t)r;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 1;
}

/* Protected by g_send_lock so multiple producers (pb poll + request
 * responses) don't interleave messages. */
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

static int send_message(int fd, uint32_t msg_type,
                        NSString *format_uti, NSData *data) {
    NSData *fmtData = format_uti
        ? [format_uti dataUsingEncoding:NSUTF8StringEncoding]
        : nil;
    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(msg_type),
        .format_len = htonl((uint32_t)fmtData.length),
        .data_size  = htonl((uint32_t)data.length),
    };

    pthread_mutex_lock(&g_send_lock);
    int rc = write_full(fd, &h, sizeof(h));
    if (rc > 0 && fmtData.length) rc = write_full(fd, fmtData.bytes, fmtData.length);
    if (rc > 0 && data.length)    rc = write_full(fd, data.bytes, data.length);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

/* Forward declarations — used by the item provider below. */
static int send_data_req_v2(int fd, uint32_t seq, uint32_t itemIndex,
                            NSString *uti);
static int write_full(int fd, const void *buf, size_t n);
static NSString *alloc_tree_cache_dir(void);

/* Bumped each time we overwrite the guest pasteboard with a fresh set of
 * lazy items. Providers stamp their generation at creation and bail if it
 * no longer matches — keeps stale callbacks from firing unnecessary
 * fetches for clipboards the user has since replaced. */
static _Atomic uint64_t g_lazy_generation = 1;

/* ---- Per-item provider ----
 * One instance per NSPasteboardItem we write. Each knows which item index
 * on the host side it represents and serves ALL types for that item —
 * file-url goes through the file-tree fetch path, everything else via
 * the generic DATA_REQ_V2 + body accumulator. Per-item independence means
 * no shared state between items, no coordination, no Finder-probe cascade
 * triggering N fetches: probes hit each item's provider individually and
 * each fetch is scoped to just that one item's payload. */
@interface AsbClipItemProvider : NSObject <NSPasteboardItemDataProvider>
@property (nonatomic, assign) uint64_t generation;
@property (nonatomic, assign) uint32_t itemIndex;
@property (nonatomic, strong) NSDictionary<NSString *, NSNumber *> *sizeHints;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSData *> *cache;
@property (nonatomic, strong) NSLock *cacheLock;
@end

@implementation AsbClipItemProvider
- (instancetype)init {
    if ((self = [super init])) {
        _generation = atomic_load(&g_lazy_generation);
        _cache      = [NSMutableDictionary dictionary];
        _cacheLock  = [NSLock new];
    }
    return self;
}

/* Generic typed fetch: send DATA_REQ_V2 with (seq, itemIndex, uti), wait
 * for the accumulated body (either single DATA_RESP_V2 or chunks + END). */
- (nullable NSData *)fetchTypedData:(NSString *)type
                            timeout:(NSTimeInterval)timeout {
    int fd = g_client_sock;
    if (fd < 0) return nil;
    AsbClipPending *p = [AsbClipPending new];
    p.seqId = alloc_seq();
    pending_register(p);
    if (send_data_req_v2(fd, p.seqId, self.itemIndex, type) <= 0) {
        pending_remove(p.seqId);
        return nil;
    }
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    [p.cond lock];
    while (!p.completed) {
        if (![p.cond waitUntilDate:deadline]) break;
    }
    BOOL ok = p.completed && !p.failed;
    NSData *result = ok ? [p.body copy] : nil;
    [p.cond unlock];
    pending_remove(p.seqId);
    return result;
}

/* File-URL fetch: send DATA_REQ_V2, accumulate into a cache dir, return
 * the first top-level URL in the resulting subtree. Each item is one
 * top-level file or directory, so exactly one URL comes back. */
- (nullable NSURL *)fetchFileURLWithTimeout:(NSTimeInterval)timeout {
    int fd = g_client_sock;
    if (fd < 0) return nil;
    AsbClipPending *p = [AsbClipPending new];
    p.seqId = alloc_seq();
    p.fileTreeDir = alloc_tree_cache_dir();
    p.fileCountExpected = INT32_MAX;
    pending_register(p);
    if (send_data_req_v2(fd, p.seqId, self.itemIndex,
                         NSPasteboardTypeFileURL) <= 0) {
        pending_remove(p.seqId);
        return nil;
    }
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    [p.cond lock];
    while (!p.completed) {
        if (![p.cond waitUntilDate:deadline]) break;
    }
    BOOL ok = p.completed && !p.failed;
    NSArray<NSURL *> *urls = ok ? [p.fileTreeUrls copy] : nil;
    [p.cond unlock];
    if (!ok && p.fileTreeDir) {
        [NSFileManager.defaultManager removeItemAtPath:p.fileTreeDir error:nil];
    }
    pending_remove(p.seqId);
    return urls.firstObject;
}

- (void)pasteboard:(NSPasteboard *)pb item:(NSPasteboardItem *)item
provideDataForType:(NSString *)type {
    if (self.generation != atomic_load(&g_lazy_generation)) {
        clip_log(@"provider: stale gen for item %u type %@",
                 self.itemIndex, type);
        return;
    }
    [self.cacheLock lock];
    NSData *cached = self.cache[type];
    [self.cacheLock unlock];
    if (cached) {
        [item setData:cached forType:type];
        return;
    }

    uint64_t hint = self.sizeHints[type].unsignedLongLongValue;
    NSData *result = nil;

    if ([type isEqualToString:NSPasteboardTypeFileURL]) {
        NSTimeInterval ftimeout = hint > 1024ULL * 1024 * 1024 ? 600.0
                                : hint > 100ULL  * 1024 * 1024 ? 300.0
                                : 60.0;
        clip_log(@"provider: fetching file-url item=%u (hint=%llu, timeout=%.0fs)",
                 self.itemIndex, (unsigned long long)hint, ftimeout);
        NSURL *url = [self fetchFileURLWithTimeout:ftimeout];
        if (!url) return;
        result = [url.absoluteString dataUsingEncoding:NSUTF8StringEncoding];
    } else {
        NSTimeInterval timeout = hint > 10ULL * 1024 * 1024 ? 60.0
                               : hint > 1ULL  * 1024 * 1024 ? 10.0
                               : 2.0;
        clip_log(@"provider: fetching %@ item=%u (hint=%llu, timeout=%.0fs)",
                 type, self.itemIndex, (unsigned long long)hint, timeout);
        result = [self fetchTypedData:type timeout:timeout];
    }
    if (!result) {
        clip_log(@"provider: fetch failed for item %u %@", self.itemIndex, type);
        return;
    }
    [self.cacheLock lock];
    self.cache[type] = result;
    [self.cacheLock unlock];
    [item setData:result forType:type];
}
@end

/* Allocate a fresh cache-dir subfolder. */
static NSString *alloc_tree_cache_dir(void) {
    NSString *sub = [g_cache_base stringByAppendingPathComponent:
                     NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtPath:sub
                            withIntermediateDirectories:YES
                                             attributes:nil error:nil];
    return sub;
}

/* Build N NSPasteboardItems mirroring the host's per-item structure and
 * atomically replace the guest pasteboard. Each item's provider owns its
 * own itemIndex and knows nothing about siblings — data arrives on demand
 * scoped to one item. This matches AppKit's model of `writeObjects:` over
 * an array of NSURL/NSString/... producing one pasteboard item each. */
static void write_lazy_pasteboard(NSArray<NSDictionary *> *items) {
    atomic_fetch_add(&g_lazy_generation, 1);
    uint64_t gen = atomic_load(&g_lazy_generation);

    NSMutableArray *writables = [NSMutableArray array];
    for (NSUInteger i = 0; i < items.count; i++) {
        NSDictionary *entry = items[i];
        NSArray<NSString *> *types = entry[@"types"];
        NSDictionary<NSString *, NSNumber *> *hints = entry[@"hints"];
        if (types.count == 0) continue;

        AsbClipItemProvider *provider = [AsbClipItemProvider new];
        provider.generation = gen;
        provider.itemIndex  = (uint32_t)i;
        provider.sizeHints  = hints;

        NSPasteboardItem *pi = [NSPasteboardItem new];
        [pi setDataProvider:provider forTypes:types];
        [writables addObject:pi];
    }

    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb writeObjects:writables];
    atomic_store(&g_suppress_change, pb.changeCount);
    clip_log(@"lazy pasteboard written (gen=%llu, %u items, cc=%ld)",
             (unsigned long long)gen,
             (unsigned)writables.count, (long)pb.changeCount);
}

/* ---- V2 message send helpers ----
 * V2 bodies are: [seq_id (4 bytes BE)] + V1-equivalent payload.
 * Sends are atomic under g_send_lock so multiple threads can issue
 * requests without interleaving chunks. */

static int send_message_v2(int fd, uint32_t msg_type, uint32_t seq,
                           NSString *format_uti, NSData *data) {
    NSData *fmtData = format_uti
        ? [format_uti dataUsingEncoding:NSUTF8StringEncoding] : nil;
    uint32_t body_size = 4 + (uint32_t)data.length;
    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(msg_type),
        .format_len = htonl((uint32_t)fmtData.length),
        .data_size  = htonl(body_size),
    };
    uint32_t seqBE = htonl(seq);
    pthread_mutex_lock(&g_send_lock);
    int rc = write_full(fd, &h, sizeof(h));
    if (rc > 0 && fmtData.length) rc = write_full(fd, fmtData.bytes, fmtData.length);
    if (rc > 0)                   rc = write_full(fd, &seqBE, 4);
    if (rc > 0 && data.length)    rc = write_full(fd, data.bytes, data.length);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

/* Body: seq (4) + item_index (4). uti travels in the header's format field. */
static int send_data_req_v2(int fd, uint32_t seq, uint32_t itemIndex,
                            NSString *uti) {
    NSData *fmt = uti ? [uti dataUsingEncoding:NSUTF8StringEncoding] : nil;
    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(CLIP_MSG_FORMAT_DATA_REQ_V2),
        .format_len = htonl((uint32_t)fmt.length),
        .data_size  = htonl(8),
    };
    uint32_t seqBE = htonl(seq);
    uint32_t idxBE = htonl(itemIndex);
    pthread_mutex_lock(&g_send_lock);
    int rc = write_full(fd, &h, sizeof(h));
    if (rc > 0 && fmt.length) rc = write_full(fd, fmt.bytes, fmt.length);
    if (rc > 0) rc = write_full(fd, &seqBE, 4);
    if (rc > 0) rc = write_full(fd, &idxBE, 4);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

/* Read the seq_id prefix from a V2 body. On success, decrements
 * h.data_size by 4 so the caller can read the remaining payload. */
static BOOL read_v2_seq(int fd, ClipHeader *h, uint32_t *outSeq) {
    if (h->data_size < 4) return NO;
    uint32_t be = 0;
    if (read_full(fd, &be, 4) <= 0) return NO;
    *outSeq = ntohl(be);
    h->data_size -= 4;
    return YES;
}

/* ---- Pasteboard helpers ---- */

/* Filter NSPasteboard types → UTI strings we actually transport. */
static NSArray<NSString *> *wanted_formats(NSPasteboard *pb) {
    static NSArray *kTransport;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        kTransport = @[
            NSPasteboardTypeFileURL,     /* prefer files first */
            NSPasteboardTypePNG,
            NSPasteboardTypeTIFF,
            NSPasteboardTypeRTF,
            NSPasteboardTypeHTML,
            NSPasteboardTypeString,
        ];
    });
    NSMutableArray *out = [NSMutableArray array];
    NSArray *actual = pb.types ?: @[];
    for (NSString *want in kTransport) {
        if ([actual containsObject:want]) [out addObject:want];
    }
    /* Don't send file-url alongside PNG/TIFF even if both are offered —
     * they carry the same semantic on macOS (Finder does that for screen
     * captures). Files win. */
    if ([out containsObject:NSPasteboardTypeFileURL]) {
        [out removeObject:NSPasteboardTypePNG];
        [out removeObject:NSPasteboardTypeTIFF];
    }
    return out;
}

/* ---- Outbound: announce local pasteboard change ---- */

static void send_format_list(int fd, NSArray<NSString *> *utis) {
    NSMutableData *buf = [NSMutableData data];
    uint32_t count = htonl((uint32_t)utis.count);
    [buf appendBytes:&count length:4];
    for (NSString *uti in utis) {
        NSData *u = [uti dataUsingEncoding:NSUTF8StringEncoding];
        uint32_t len = htonl((uint32_t)u.length);
        [buf appendBytes:&len length:4];
        [buf appendData:u];
    }
    if (send_message(fd, CLIP_MSG_FORMAT_LIST, nil, buf) <= 0) {
        clip_log(@"send FORMAT_LIST failed");
    } else {
        clip_log(@"sent FORMAT_LIST (%d formats)", (int)utis.count);
    }
}

/* ---- Outbound: respond to DATA_REQ ---- */

static NSData *read_file_url_payload_from_pb(NSPasteboard *pb) {
    /* Enumerate each top-level URL (file or directory), then walk
     * descendants and emit flat list of FILE_DATA entries. Here we only
     * write the COUNT; the FILE_DATA messages follow as separate sends. */
    NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[NSURL.class]
                                                options:@{}] ?: @[];
    NSMutableArray<NSURL *> *valid = [NSMutableArray array];
    for (NSURL *u in urls) if (u.isFileURL) [valid addObject:u];

    /* Count total entries (files + dirs, recursive). */
    uint32_t total = 0;
    for (NSURL *u in valid) {
        total++;
        NSNumber *isDir = nil;
        [u getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if (isDir.boolValue) {
            NSDirectoryEnumerator *en = [NSFileManager.defaultManager
                enumeratorAtURL:u
                includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                                   options:0 errorHandler:nil];
            for (NSURL *__unused child in en) total++;
        }
    }
    uint32_t be = htonl(total);
    return [NSData dataWithBytes:&be length:4];
}

/* Streaming FILE_DATA sender — matches Windows wire format:
 *   ClipHeader  (data_size = sizeof(ClipFileInfo) + path_len)
 *   ClipFileInfo (path_len, file_size, is_directory)
 *   path bytes
 *   file_size raw bytes streamed in CLIP_FILE_CHUNK pieces
 * The raw file bytes are NOT counted in data_size. Held under g_send_lock
 * so the poll thread's FORMAT_LIST can't interleave mid-stream. */
static int send_file_entry(int fd, NSString *relPath, BOOL isDir,
                           uint64_t fileSize, int openFd) {
    NSData *pathUTF8 = [relPath dataUsingEncoding:NSUTF8StringEncoding];
    uint32_t path_len = (uint32_t)pathUTF8.length;
    uint32_t data_size = (uint32_t)sizeof(ClipFileInfo) + path_len;

    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(CLIP_MSG_FILE_DATA),
        .format_len = htonl(0),
        .data_size  = htonl(data_size),
    };
    ClipFileInfo info = {
        .path_len     = htonl(path_len),
        .file_size    = CFSwapInt64HostToBig(isDir ? 0ULL : fileSize),
        .is_directory = htonl(isDir ? 1u : 0u),
    };

    int rc = 1;
    pthread_mutex_lock(&g_send_lock);
    if (rc > 0) rc = write_full(fd, &h, sizeof(h));
    if (rc > 0) rc = write_full(fd, &info, sizeof(info));
    if (rc > 0 && path_len) rc = write_full(fd, pathUTF8.bytes, path_len);

    if (rc > 0 && !isDir && fileSize > 0 && openFd >= 0) {
        uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
        if (!chunk) { rc = -1; }
        uint64_t remaining = fileSize;
        while (rc > 0 && remaining > 0) {
            size_t want = remaining > CLIP_FILE_CHUNK
                ? CLIP_FILE_CHUNK : (size_t)remaining;
            ssize_t got = read(openFd, chunk, want);
            if (got <= 0) { rc = -1; break; }
            if (write_full(fd, chunk, (size_t)got) <= 0) { rc = -1; break; }
            remaining -= (uint64_t)got;
        }
        if (chunk) free(chunk);
    }
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

static int send_file_tree(int fd, NSURL *top) {
    NSString *baseName = top.lastPathComponent;
    NSNumber *isDir = nil;
    [top getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (!isDir.boolValue) {
        int ffd = open(top.path.fileSystemRepresentation, O_RDONLY);
        if (ffd < 0) {
            clip_log(@"open %@: %s", top.path, strerror(errno));
            /* Still emit a zero-length entry so receiver count stays in sync. */
            return send_file_entry(fd, baseName, NO, 0, -1);
        }
        struct stat st;
        uint64_t sz = (fstat(ffd, &st) == 0) ? (uint64_t)st.st_size : 0;
        int rc = send_file_entry(fd, baseName, NO, sz, ffd);
        close(ffd);
        return rc;
    }

    if (send_file_entry(fd, baseName, YES, 0, -1) <= 0) return -1;
    NSDirectoryEnumerator *en = [NSFileManager.defaultManager
        enumeratorAtURL:top
        includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                           options:0 errorHandler:nil];
    for (NSURL *child in en) {
        NSString *rel = [baseName stringByAppendingPathComponent:
                         [child.path substringFromIndex:top.path.length + 1]];
        NSNumber *cIsDir = nil;
        [child getResourceValue:&cIsDir forKey:NSURLIsDirectoryKey error:nil];
        if (cIsDir.boolValue) {
            if (send_file_entry(fd, rel, YES, 0, -1) <= 0) return -1;
        } else {
            int ffd = open(child.path.fileSystemRepresentation, O_RDONLY);
            uint64_t sz = 0;
            if (ffd >= 0) {
                struct stat st;
                if (fstat(ffd, &st) == 0) sz = (uint64_t)st.st_size;
            }
            int rc = send_file_entry(fd, rel, NO, sz, ffd);
            if (ffd >= 0) close(ffd);
            if (rc <= 0) return -1;
        }
    }
    return 1;
}

/* ---- V2 outbound response helpers (guest → host when host pastes) ---- */

/* Filter one pasteboard item's types down to what we transport, with
 * file-url shadowing png/tiff (Finder adds those as previews for a file
 * copy — we only want the file). */
static NSArray<NSString *> *transport_types_for_item(NSPasteboardItem *item) {
    static NSArray *kTransport;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        kTransport = @[NSPasteboardTypeFileURL, NSPasteboardTypePNG,
                       NSPasteboardTypeTIFF, NSPasteboardTypeRTF,
                       NSPasteboardTypeHTML, NSPasteboardTypeString];
    });
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSString *want in kTransport) {
        if ([item.types containsObject:want]) [out addObject:want];
    }
    if ([out containsObject:NSPasteboardTypeFileURL]) {
        [out removeObject:NSPasteboardTypePNG];
        [out removeObject:NSPasteboardTypeTIFF];
    }
    return out;
}

/* Per-type size hint for one pasteboard item. For file-url, sums the
 * subtree byte total; for everything else, the data length. */
static uint64_t size_hint_for_item_type(NSPasteboardItem *item,
                                          NSString *uti) {
    if ([uti isEqualToString:NSPasteboardTypeFileURL]) {
        NSString *urlStr = [item stringForType:uti];
        NSURL *url = urlStr ? [NSURL URLWithString:urlStr] : nil;
        if (!url.isFileURL) return 0;
        NSNumber *isDir = nil;
        [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if (!isDir.boolValue) {
            NSNumber *sz = nil;
            if ([url getResourceValue:&sz forKey:NSURLFileSizeKey error:nil])
                return sz.unsignedLongLongValue;
            return 0;
        }
        NSDirectoryEnumerator *en = [NSFileManager.defaultManager
            enumeratorAtURL:url
            includingPropertiesForKeys:@[NSURLFileSizeKey,
                                         NSURLIsDirectoryKey]
                               options:0 errorHandler:nil];
        uint64_t total = 0;
        for (NSURL *c in en) {
            NSNumber *cIsDir = nil;
            [c getResourceValue:&cIsDir forKey:NSURLIsDirectoryKey error:nil];
            if (cIsDir.boolValue) continue;
            NSNumber *sz = nil;
            if ([c getResourceValue:&sz forKey:NSURLFileSizeKey error:nil])
                total += sz.unsignedLongLongValue;
        }
        return total;
    }
    NSData *d = [item dataForType:uti];
    return (uint64_t)d.length;
}

/* Build the file-url per-item meta: name_len, name, size, is_dir. */
static NSData *file_meta_for_item(NSPasteboardItem *item) {
    NSString *urlStr = [item stringForType:NSPasteboardTypeFileURL];
    NSURL *url = urlStr ? [NSURL URLWithString:urlStr] : nil;
    if (!url.isFileURL) return nil;
    NSString *name = url.lastPathComponent ?: @"";
    NSData *nb = [name dataUsingEncoding:NSUTF8StringEncoding];
    NSNumber *isDir = nil;
    [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    uint64_t sz = 0;
    if (!isDir.boolValue) {
        NSNumber *szNum = nil;
        if ([url getResourceValue:&szNum forKey:NSURLFileSizeKey error:nil])
            sz = szNum.unsignedLongLongValue;
    }
    NSMutableData *m = [NSMutableData data];
    uint32_t nlen = htonl((uint32_t)nb.length);
    uint64_t szBE = CFSwapInt64HostToBig(sz);
    uint32_t dBE = htonl(isDir.boolValue ? 1u : 0u);
    [m appendBytes:&nlen length:4];
    [m appendData:nb];
    [m appendBytes:&szBE length:8];
    [m appendBytes:&dBE length:4];
    return m;
}

/* Send FORMAT_LIST_V2 for every item on the pasteboard. Body is:
 *   uint32 item_count
 *   per-item: uint32 type_count, N × (uti, size_hint, meta)
 * Items with no transportable types are skipped. */
static void send_format_list_v2(int fd, NSPasteboard *pb) {
    NSArray<NSPasteboardItem *> *items = pb.pasteboardItems ?: @[];
    NSMutableData *itemsBody = [NSMutableData data];
    uint32_t itemCount = 0;

    for (NSPasteboardItem *item in items) {
        NSArray<NSString *> *utis = transport_types_for_item(item);
        if (utis.count == 0) continue;
        itemCount++;

        uint32_t tc = htonl((uint32_t)utis.count);
        [itemsBody appendBytes:&tc length:4];
        for (NSString *uti in utis) {
            NSData *u = [uti dataUsingEncoding:NSUTF8StringEncoding];
            uint32_t ulen = htonl((uint32_t)u.length);
            uint64_t hint = size_hint_for_item_type(item, uti);
            uint64_t hintBE = CFSwapInt64HostToBig(hint);
            NSData *meta = nil;
            if ([uti isEqualToString:NSPasteboardTypeFileURL]) {
                meta = file_meta_for_item(item);
            }
            uint32_t mlen = htonl((uint32_t)meta.length);
            [itemsBody appendBytes:&ulen length:4];
            [itemsBody appendData:u];
            [itemsBody appendBytes:&hintBE length:8];
            [itemsBody appendBytes:&mlen length:4];
            if (meta.length) [itemsBody appendData:meta];
        }
    }

    NSMutableData *body = [NSMutableData data];
    uint32_t icBE = htonl(itemCount);
    [body appendBytes:&icBE length:4];
    [body appendData:itemsBody];

    if (send_message(fd, CLIP_MSG_FORMAT_LIST_V2, nil, body) <= 0) {
        clip_log(@"send FORMAT_LIST_V2 failed");
    } else {
        clip_log(@"sent FORMAT_LIST_V2 (%u items)", itemCount);
    }
}

/* Chunked V2 DATA_RESP sender — mirrors host's -sendDataRespV2:. Small
 * payloads go in one DATA_RESP_V2; large ones are empty DATA_RESP_V2
 * followed by FORMAT_DATA_CHUNK × N + FORMAT_DATA_END. */
static int send_data_resp_v2(int fd, uint32_t seq, NSString *fmt, NSData *bytes) {
    if ((uint32_t)bytes.length <= CLIP_LARGE_THRESHOLD) {
        return send_message_v2(fd, CLIP_MSG_FORMAT_DATA_RESP_V2, seq, fmt, bytes);
    }
    int rc = send_message_v2(fd, CLIP_MSG_FORMAT_DATA_RESP_V2, seq, fmt, nil);
    if (rc <= 0) return rc;
    const uint8_t *p = bytes.bytes;
    NSUInteger remaining = bytes.length;
    while (remaining > 0) {
        NSUInteger chunk = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : remaining;
        NSData *slice = [NSData dataWithBytesNoCopy:(void *)p length:chunk
                                       freeWhenDone:NO];
        rc = send_message_v2(fd, CLIP_MSG_FORMAT_DATA_CHUNK, seq, nil, slice);
        if (rc <= 0) return rc;
        p += chunk;
        remaining -= chunk;
    }
    return send_message_v2(fd, CLIP_MSG_FORMAT_DATA_END, seq, nil, nil);
}

/* V2 FILE_DATA entry: seq + ClipFileInfo + path + raw file bytes. */
static int send_file_entry_v2(int fd, uint32_t seq, NSString *relPath,
                               BOOL isDir, uint64_t fileSize, int openFd) {
    NSData *pathUTF8 = [relPath dataUsingEncoding:NSUTF8StringEncoding];
    uint32_t path_len = (uint32_t)pathUTF8.length;
    uint32_t data_size = 4 + (uint32_t)sizeof(ClipFileInfo) + path_len;

    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(CLIP_MSG_FILE_DATA_V2),
        .format_len = htonl(0),
        .data_size  = htonl(data_size),
    };
    ClipFileInfo info = {
        .path_len     = htonl(path_len),
        .file_size    = CFSwapInt64HostToBig(isDir ? 0ULL : fileSize),
        .is_directory = htonl(isDir ? 1u : 0u),
    };
    uint32_t seqBE = htonl(seq);

    int rc = 1;
    pthread_mutex_lock(&g_send_lock);
    if (rc > 0) rc = write_full(fd, &h, sizeof(h));
    if (rc > 0) rc = write_full(fd, &seqBE, 4);
    if (rc > 0) rc = write_full(fd, &info, sizeof(info));
    if (rc > 0 && path_len) rc = write_full(fd, pathUTF8.bytes, path_len);

    if (rc > 0 && !isDir && fileSize > 0 && openFd >= 0) {
        uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
        if (!chunk) rc = -1;
        uint64_t remaining = fileSize;
        while (rc > 0 && remaining > 0) {
            size_t want = remaining > CLIP_FILE_CHUNK
                ? CLIP_FILE_CHUNK : (size_t)remaining;
            ssize_t got = read(openFd, chunk, want);
            if (got <= 0) { rc = -1; break; }
            if (write_full(fd, chunk, (size_t)got) <= 0) { rc = -1; break; }
            remaining -= (uint64_t)got;
        }
        if (chunk) free(chunk);
    }
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

static int send_file_tree_v2(int fd, uint32_t seq, NSURL *top) {
    NSString *baseName = top.lastPathComponent;
    NSNumber *isDir = nil;
    [top getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (!isDir.boolValue) {
        int ffd = open(top.path.fileSystemRepresentation, O_RDONLY);
        if (ffd < 0) {
            return send_file_entry_v2(fd, seq, baseName, NO, 0, -1);
        }
        struct stat st;
        uint64_t sz = (fstat(ffd, &st) == 0) ? (uint64_t)st.st_size : 0;
        int rc = send_file_entry_v2(fd, seq, baseName, NO, sz, ffd);
        close(ffd);
        return rc;
    }
    if (send_file_entry_v2(fd, seq, baseName, YES, 0, -1) <= 0) return -1;
    NSDirectoryEnumerator *en = [NSFileManager.defaultManager
        enumeratorAtURL:top
        includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                           options:0 errorHandler:nil];
    for (NSURL *child in en) {
        NSString *rel = [baseName stringByAppendingPathComponent:
                         [child.path substringFromIndex:top.path.length + 1]];
        NSNumber *cIsDir = nil;
        [child getResourceValue:&cIsDir forKey:NSURLIsDirectoryKey error:nil];
        if (cIsDir.boolValue) {
            if (send_file_entry_v2(fd, seq, rel, YES, 0, -1) <= 0) return -1;
        } else {
            int ffd = open(child.path.fileSystemRepresentation, O_RDONLY);
            uint64_t sz = 0;
            if (ffd >= 0) {
                struct stat st;
                if (fstat(ffd, &st) == 0) sz = (uint64_t)st.st_size;
            }
            int rc = send_file_entry_v2(fd, seq, rel, NO, sz, ffd);
            if (ffd >= 0) close(ffd);
            if (rc <= 0) return -1;
        }
    }
    return 1;
}

/* V2 request handler: host is asking the guest for one specific item's
 * typed data. Looks up pb.pasteboardItems[itemIndex] and responds with
 * that item's data. For file-url, counts the subtree rooted at that
 * item's URL and streams a single FILE_DATA_V2 sequence covering it. */
static void handle_data_request_v2(int fd, NSString *format, uint32_t seq,
                                    uint32_t itemIndex) {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    NSArray<NSPasteboardItem *> *items = pb.pasteboardItems;
    clip_log(@"guest → host DATA_REQ_V2 seq=%u item=%u %@",
             seq, itemIndex, format);

    if (itemIndex >= items.count) {
        clip_log(@"DATA_REQ_V2 item %u out of range (count %lu)",
                 itemIndex, (unsigned long)items.count);
        send_data_resp_v2(fd, seq, format, [NSData data]);
        return;
    }
    NSPasteboardItem *item = items[itemIndex];

    if ([format isEqualToString:NSPasteboardTypeFileURL]) {
        NSString *urlStr = [item stringForType:NSPasteboardTypeFileURL];
        NSURL *url = urlStr ? [NSURL URLWithString:urlStr] : nil;
        if (!url.isFileURL) {
            uint32_t zero = htonl(0);
            send_message_v2(fd, CLIP_MSG_FORMAT_DATA_RESP_V2, seq, format,
                            [NSData dataWithBytes:&zero length:4]);
            return;
        }
        uint32_t count = 1;
        NSNumber *isDir = nil;
        [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if (isDir.boolValue) {
            NSDirectoryEnumerator *en = [NSFileManager.defaultManager
                enumeratorAtURL:url
                includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                                   options:0 errorHandler:nil];
            for (NSURL *__unused c in en) count++;
        }
        uint32_t be = htonl(count);
        send_message_v2(fd, CLIP_MSG_FORMAT_DATA_RESP_V2, seq, format,
                        [NSData dataWithBytes:&be length:4]);
        send_file_tree_v2(fd, seq, url);
        return;
    }

    NSData *d = [item dataForType:format];
    send_data_resp_v2(fd, seq, format, d ?: [NSData data]);
}

static void handle_data_request(int fd, NSString *format) {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    if ([format isEqualToString:NSPasteboardTypeFileURL]) {
        /* Special: payload is count, followed by separate FILE_DATA msgs. */
        NSData *countPayload = read_file_url_payload_from_pb(pb);
        if (send_message(fd, CLIP_MSG_FORMAT_DATA_RESP, format, countPayload) <= 0) return;
        NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[NSURL.class]
                                                    options:@{}] ?: @[];
        for (NSURL *u in urls) {
            if (!u.isFileURL) continue;
            if (send_file_tree(fd, u) <= 0) return;
        }
        clip_log(@"sent file-url (%d top-level)", (int)urls.count);
        return;
    }

    NSData *data = [pb dataForType:format];
    if (!data) data = [NSData data];
    if (send_message(fd, CLIP_MSG_FORMAT_DATA_RESP, format, data) <= 0) {
        clip_log(@"send DATA_RESP failed for %@", format);
    } else {
        clip_log(@"sent DATA_RESP %@ (%u bytes)", format, (unsigned)data.length);
    }
}

/* ---- Inbound: apply remote pasteboard ---- */

static NSURL *alloc_cache_dir(void) {
    NSString *sub = [g_cache_base stringByAppendingPathComponent:
                     NSUUID.UUID.UUIDString];
    NSError *err = nil;
    [NSFileManager.defaultManager createDirectoryAtPath:sub
                            withIntermediateDirectories:YES
                                             attributes:nil error:&err];
    return [NSURL fileURLWithPath:sub];
}

/* Stream one FILE_DATA message body:
 *   - header carries data_size = sizeof(ClipFileInfo) + path_len
 *   - file_size raw bytes follow the header (not included in data_size)
 * Directories and existing dirs under `destDir` are created as needed,
 * regular files are written incrementally in CLIP_FILE_CHUNK pieces so the
 * whole file never lives in memory. On success, if outTopLevel is not NULL
 * and this was a top-level entry (pathComponents.count == 1), receives the
 * materialized NSURL. */
static BOOL stream_file_data(int fd, ClipHeader h, NSURL *destDir,
                             NSURL **outTopLevel) {
    if (h.data_size < sizeof(ClipFileInfo)) return NO;
    ClipFileInfo info;
    if (read_full(fd, &info, sizeof(info)) <= 0) return NO;
    uint32_t path_len  = ntohl(info.path_len);
    uint64_t file_size = CFSwapInt64BigToHost(info.file_size);
    uint32_t is_dir    = ntohl(info.is_directory);
    if ((uint64_t)sizeof(info) + path_len != h.data_size) {
        clip_log(@"FILE_DATA header size mismatch");
        return NO;
    }
    char *path_buf = malloc((size_t)path_len + 1);
    if (!path_buf) return NO;
    if (path_len && read_full(fd, path_buf, path_len) <= 0) {
        free(path_buf); return NO;
    }
    path_buf[path_len] = 0;
    NSString *rel = [NSString stringWithUTF8String:path_buf];
    free(path_buf);

    if ([rel containsString:@".."]) {
        /* Drain the incoming bytes so the stream stays in sync, drop result. */
        uint8_t drain[4096];
        uint64_t rem = file_size;
        while (rem > 0) {
            size_t want = rem > sizeof(drain) ? sizeof(drain) : (size_t)rem;
            if (read_full(fd, drain, want) <= 0) return NO;
            rem -= want;
        }
        clip_log(@"FILE_DATA rejected unsafe path");
        return YES;
    }

    NSURL *target = [destDir URLByAppendingPathComponent:rel];
    if (is_dir) {
        [NSFileManager.defaultManager createDirectoryAtURL:target
                               withIntermediateDirectories:YES
                                                attributes:nil error:nil];
        if (outTopLevel && rel.pathComponents.count == 1) *outTopLevel = target;
        return YES;
    }

    [NSFileManager.defaultManager createDirectoryAtURL:[target URLByDeletingLastPathComponent]
                           withIntermediateDirectories:YES
                                            attributes:nil error:nil];
    int ffd = open(target.path.fileSystemRepresentation,
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
    if (!chunk) { if (ffd >= 0) close(ffd); return NO; }
    uint64_t remaining = file_size;
    BOOL ok = YES;
    while (remaining > 0) {
        size_t want = remaining > CLIP_FILE_CHUNK
            ? CLIP_FILE_CHUNK : (size_t)remaining;
        if (read_full(fd, chunk, want) <= 0) { ok = NO; break; }
        if (ffd >= 0) {
            const uint8_t *p = chunk;
            size_t left = want;
            while (left) {
                ssize_t w = write(ffd, p, left);
                if (w <= 0) { ok = NO; break; }
                p += w; left -= (size_t)w;
            }
            if (!ok) break;
        }
        remaining -= want;
    }
    free(chunk);
    if (ffd >= 0) close(ffd);
    if (!ok) return NO;

    if (outTopLevel && rel.pathComponents.count == 1) *outTopLevel = target;
    return YES;
}

static void apply_format_data(NSMutableDictionary *acc, NSString *format, NSData *data) {
    acc[format] = data ?: [NSData data];
}

static void write_to_pasteboard(NSDictionary *formatData, NSArray<NSURL *> *fileUrls) {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];

    if (fileUrls.count) {
        [pb writeObjects:fileUrls];
    }
    for (NSString *fmt in formatData) {
        if ([fmt isEqualToString:NSPasteboardTypeFileURL]) continue;
        NSData *d = formatData[fmt];
        if (d.length == 0) continue;
        [pb setData:d forType:fmt];
    }
    /* Record changeCount so poller swallows the echo. */
    atomic_store(&g_suppress_change, pb.changeCount);
    clip_log(@"applied pasteboard (changeCount=%ld, %d formats, %d files)",
             (long)pb.changeCount, (int)formatData.count, (int)fileUrls.count);
}

/* ---- Connection main loop ---- */

static int connect_sequence_inbound_format_list(int fd, ClipHeader h) {
    /* Payload: uint32 count + N * (uint32 len + utf8 uti). */
    NSMutableData *buf = [NSMutableData dataWithLength:h.data_size];
    if (read_full(fd, buf.mutableBytes, h.data_size) <= 0) return -1;

    const uint8_t *p = buf.bytes;
    const uint8_t *end = p + h.data_size;
    if (end - p < 4) return -1;
    uint32_t count;
    memcpy(&count, p, 4); p += 4; count = ntohl(count);

    NSMutableArray<NSString *> *utis = [NSMutableArray array];
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 4) return -1;
        uint32_t len; memcpy(&len, p, 4); p += 4; len = ntohl(len);
        if ((size_t)(end - p) < len) return -1;
        NSString *uti = [[NSString alloc] initWithBytes:p length:len
                                                encoding:NSUTF8StringEncoding];
        if (uti) [utis addObject:uti];
        p += len;
    }
    clip_log(@"recv FORMAT_LIST: %@", [utis componentsJoinedByString:@", "]);

    /* For each format, issue DATA_REQ then wait for DATA_RESP to accumulate. */
    NSMutableDictionary<NSString *, NSData *> *dataByFmt = [NSMutableDictionary dictionary];
    NSMutableArray<NSURL *> *fileUrls = [NSMutableArray array];

    for (NSString *fmt in utis) {
        if (send_message(fd, CLIP_MSG_FORMAT_DATA_REQ, fmt, nil) <= 0) return -1;

        /* Loop reading messages until we see DATA_RESP for this fmt. */
        for (;;) {
            ClipHeader rh;
            if (read_full(fd, &rh, sizeof(rh)) <= 0) return -1;
            rh.magic      = ntohl(rh.magic);
            rh.msg_type   = ntohl(rh.msg_type);
            rh.format_len = ntohl(rh.format_len);
            rh.data_size  = ntohl(rh.data_size);
            if (rh.magic != CLIP_MAGIC) return -1;
            if (rh.data_size > CLIP_MAX_PAYLOAD) return -1;

            NSString *respFmt = nil;
            if (rh.format_len) {
                char *fb = malloc(rh.format_len + 1);
                if (!fb) return -1;
                if (read_full(fd, fb, rh.format_len) <= 0) { free(fb); return -1; }
                fb[rh.format_len] = 0;
                respFmt = [NSString stringWithUTF8String:fb];
                free(fb);
            }

            if (rh.msg_type == CLIP_MSG_FORMAT_DATA_RESP) {
                if ([fmt isEqualToString:NSPasteboardTypeFileURL]) {
                    /* Payload is uint32 count, followed by count separate
                     * FILE_DATA messages. */
                    uint8_t countBuf[4] = {0};
                    if (rh.data_size >= 4) {
                        if (read_full(fd, countBuf, 4) <= 0) return -1;
                        if (rh.data_size > 4) {
                            NSMutableData *trail = [NSMutableData dataWithLength:rh.data_size - 4];
                            read_full(fd, trail.mutableBytes, rh.data_size - 4);
                        }
                    }
                    uint32_t n; memcpy(&n, countBuf, 4); n = ntohl(n);
                    NSURL *dest = alloc_cache_dir();
                    for (uint32_t i = 0; i < n; i++) {
                        ClipHeader fh;
                        if (read_full(fd, &fh, sizeof(fh)) <= 0) return -1;
                        fh.magic     = ntohl(fh.magic);
                        fh.msg_type  = ntohl(fh.msg_type);
                        fh.format_len= ntohl(fh.format_len);
                        fh.data_size = ntohl(fh.data_size);
                        if (fh.magic != CLIP_MAGIC || fh.msg_type != CLIP_MSG_FILE_DATA)
                            return -1;
                        NSURL *topLevel = nil;
                        if (!stream_file_data(fd, fh, dest, &topLevel)) return -1;
                        if (topLevel) [fileUrls addObject:topLevel];
                    }
                    break;
                }

                NSMutableData *body = nil;
                if (rh.data_size) {
                    body = [NSMutableData dataWithLength:rh.data_size];
                    if (read_full(fd, body.mutableBytes, rh.data_size) <= 0) return -1;
                }
                apply_format_data(dataByFmt, respFmt ?: fmt, body);
                break;
            }

            /* Something else while we're waiting — skip its body and keep going. */
            if (rh.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:rh.data_size];
                read_full(fd, trash.mutableBytes, rh.data_size);
            }
        }
    }

    write_to_pasteboard(dataByFmt, fileUrls);
    return 1;
}

static int handle_inbound(int fd) {
    ClipHeader h;
    if (read_full(fd, &h, sizeof(h)) <= 0) return -1;
    h.magic      = ntohl(h.magic);
    h.msg_type   = ntohl(h.msg_type);
    h.format_len = ntohl(h.format_len);
    h.data_size  = ntohl(h.data_size);
    if (h.magic != CLIP_MAGIC)            return -1;
    if (h.data_size > CLIP_MAX_PAYLOAD)   return -1;

    NSString *format = nil;
    if (h.format_len) {
        char *fb = malloc(h.format_len + 1);
        if (!fb) return -1;
        if (read_full(fd, fb, h.format_len) <= 0) { free(fb); return -1; }
        fb[h.format_len] = 0;
        format = [NSString stringWithUTF8String:fb];
        free(fb);
    }

    if (h.msg_type == CLIP_MSG_FORMAT_LIST) {
        return connect_sequence_inbound_format_list(fd, h);
    }
    if (h.msg_type == CLIP_MSG_FORMAT_DATA_REQ) {
        if (h.data_size) {
            NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
            read_full(fd, trash.mutableBytes, h.data_size);
        }
        if (format) handle_data_request(fd, format);
        return 1;
    }

    /* V2 request from host asking the guest for a specific item's data.
     * Body = seq (4) + item_index (4). */
    if (h.msg_type == CLIP_MSG_FORMAT_DATA_REQ_V2) {
        if (h.data_size < 8) {
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                read_full(fd, trash.mutableBytes, h.data_size);
            }
            return 1;
        }
        uint32_t seqBE = 0, idxBE = 0;
        if (read_full(fd, &seqBE, 4) <= 0) return -1;
        if (read_full(fd, &idxBE, 4) <= 0) return -1;
        uint32_t remaining = h.data_size - 8;
        if (remaining) {
            NSMutableData *trash = [NSMutableData dataWithLength:remaining];
            read_full(fd, trash.mutableBytes, remaining);
        }
        uint32_t seq = ntohl(seqBE);
        uint32_t itemIndex = ntohl(idxBE);
        if (format) handle_data_request_v2(fd, format, seq, itemIndex);
        return 1;
    }

    /* ---- V2 lazy path (per-item) ----
     * Body: uint32 item_count, then per-item:
     *   uint32 type_count
     *   N × (uint32 uti_len, uti, uint64 size_hint, uint32 meta_len, meta)
     * Produces an NSArray<NSDictionary *> where each entry describes one
     * pasteboard item to create. */
    if (h.msg_type == CLIP_MSG_FORMAT_LIST_V2) {
        if (h.data_size < 4) {
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                read_full(fd, trash.mutableBytes, h.data_size);
            }
            return 1;
        }
        NSMutableData *buf = [NSMutableData dataWithLength:h.data_size];
        if (read_full(fd, buf.mutableBytes, h.data_size) <= 0) return -1;
        const uint8_t *p = buf.bytes;
        const uint8_t *end = p + h.data_size;

        uint32_t itemCount = 0;
        memcpy(&itemCount, p, 4); p += 4; itemCount = ntohl(itemCount);
        NSMutableArray<NSDictionary *> *items = [NSMutableArray array];

        for (uint32_t i = 0; i < itemCount; i++) {
            if (end - p < 4) break;
            uint32_t tc; memcpy(&tc, p, 4); p += 4; tc = ntohl(tc);

            NSMutableArray<NSString *> *types = [NSMutableArray array];
            NSMutableDictionary<NSString *, NSNumber *> *hints =
                [NSMutableDictionary dictionary];

            for (uint32_t j = 0; j < tc; j++) {
                if (end - p < 4) goto parse_done;
                uint32_t ulen; memcpy(&ulen, p, 4); p += 4; ulen = ntohl(ulen);
                if ((size_t)(end - p) < ulen + 8 + 4) goto parse_done;
                NSString *uti = [[NSString alloc] initWithBytes:p length:ulen
                                                       encoding:NSUTF8StringEncoding];
                p += ulen;
                uint64_t hintBE; memcpy(&hintBE, p, 8); p += 8;
                uint64_t hint = CFSwapInt64BigToHost(hintBE);
                uint32_t mlen; memcpy(&mlen, p, 4); p += 4; mlen = ntohl(mlen);
                if ((size_t)(end - p) < mlen) goto parse_done;
                /* We don't currently consume the meta bytes — they're
                 * reserved for future use (e.g. showing file names in a
                 * paste-preview tooltip). Skip them. */
                p += mlen;
                if (uti) {
                    [types addObject:uti];
                    hints[uti] = @(hint);
                }
            }
            if (types.count > 0) {
                [items addObject:@{@"types": [types copy],
                                   @"hints": [hints copy]}];
            }
        }
    parse_done:

        clip_log(@"recv FORMAT_LIST_V2: %u items", (unsigned)items.count);
        if (items.count > 0) {
            NSArray *itemsCopy = [items copy];
            dispatch_async(dispatch_get_main_queue(), ^{
                write_lazy_pasteboard(itemsCopy);
            });
        }
        return 1;
    }
    if (h.msg_type == CLIP_MSG_FORMAT_DATA_RESP_V2 ||
        h.msg_type == CLIP_MSG_FORMAT_DATA_CHUNK   ||
        h.msg_type == CLIP_MSG_FORMAT_DATA_END     ||
        h.msg_type == CLIP_MSG_FILE_DATA_V2) {
        uint32_t seq = 0;
        if (!read_v2_seq(fd, &h, &seq)) {
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                read_full(fd, trash.mutableBytes, h.data_size);
            }
            return 1;
        }
        AsbClipPending *p = pending_lookup(seq);
        if (!p) {
            /* No waiter — most likely cancelled. Drain body. */
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                read_full(fd, trash.mutableBytes, h.data_size);
            }
            return 1;
        }

        if (h.msg_type == CLIP_MSG_FORMAT_DATA_RESP_V2) {
            /* For file-url: body is 4-byte file count. Do NOT mark
             * completed — caller expects N FILE_DATA_V2 messages to
             * follow, tracked via p.fileCountExpected. For anything
             * else, this is the single-shot body. */
            if (p.fileTreeDir) {
                if (h.data_size < 4) {
                    if (h.data_size) {
                        NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                        read_full(fd, trash.mutableBytes, h.data_size);
                    }
                    [p.cond lock];
                    p.failed = YES; p.completed = YES;
                    [p.cond broadcast];
                    [p.cond unlock];
                    return 1;
                }
                uint32_t cnt = 0;
                if (read_full(fd, &cnt, 4) <= 0) return -1;
                cnt = ntohl(cnt);
                if (h.data_size > 4) {
                    NSMutableData *trash = [NSMutableData dataWithLength:h.data_size - 4];
                    if (read_full(fd, trash.mutableBytes, h.data_size - 4) <= 0)
                        return -1;
                }
                [p.cond lock];
                p.fileCountExpected = (int32_t)cnt;
                if (cnt == 0) {
                    p.completed = YES;
                    [p.cond broadcast];
                }
                [p.cond unlock];
                return 1;
            }

            /* Non-file: whole body in one message. */
            NSMutableData *buf = nil;
            if (h.data_size) {
                buf = [NSMutableData dataWithLength:h.data_size];
                if (read_full(fd, buf.mutableBytes, h.data_size) <= 0) return -1;
            }
            [p.cond lock];
            if (buf) [p.body appendData:buf];
            p.completed = YES;
            [p.cond broadcast];
            [p.cond unlock];
            return 1;
        }
        if (h.msg_type == CLIP_MSG_FORMAT_DATA_CHUNK) {
            NSMutableData *buf = nil;
            if (h.data_size) {
                buf = [NSMutableData dataWithLength:h.data_size];
                if (read_full(fd, buf.mutableBytes, h.data_size) <= 0) return -1;
            }
            [p.cond lock];
            if (buf) [p.body appendData:buf];
            [p.cond unlock];
            return 1;
        }
        if (h.msg_type == CLIP_MSG_FORMAT_DATA_END) {
            [p.cond lock];
            p.completed = YES;
            [p.cond broadcast];
            [p.cond unlock];
            return 1;
        }
        if (h.msg_type == CLIP_MSG_FILE_DATA_V2) {
            if (h.data_size < sizeof(ClipFileInfo)) return -1;
            ClipFileInfo info;
            if (read_full(fd, &info, sizeof(info)) <= 0) return -1;
            uint32_t path_len  = ntohl(info.path_len);
            uint64_t file_size = CFSwapInt64BigToHost(info.file_size);
            uint32_t is_dir    = ntohl(info.is_directory);
            if ((uint64_t)sizeof(info) + path_len != h.data_size) return -1;

            char *pathBuf = malloc((size_t)path_len + 1);
            if (!pathBuf) return -1;
            if (path_len && read_full(fd, pathBuf, path_len) <= 0) {
                free(pathBuf); return -1;
            }
            pathBuf[path_len] = 0;
            NSString *relPath = [NSString stringWithUTF8String:pathBuf];
            free(pathBuf);

            /* Path traversal guard. */
            if ([relPath containsString:@".."]) {
                uint64_t rem = file_size;
                uint8_t trash[4096];
                while (rem > 0) {
                    size_t w = rem > sizeof(trash) ? sizeof(trash) : (size_t)rem;
                    if (read_full(fd, trash, w) <= 0) return -1;
                    rem -= w;
                }
                return 1;
            }

            /* Two paths: multi-file tree (p.fileTreeDir set) or single
             * file into p.fileFd / p.body. */
            if (p.fileTreeDir) {
                NSString *target = [p.fileTreeDir
                    stringByAppendingPathComponent:relPath];
                if (is_dir) {
                    [NSFileManager.defaultManager createDirectoryAtPath:target
                                        withIntermediateDirectories:YES
                                                         attributes:nil error:nil];
                } else {
                    [NSFileManager.defaultManager createDirectoryAtPath:
                        [target stringByDeletingLastPathComponent]
                                        withIntermediateDirectories:YES
                                                         attributes:nil error:nil];
                    int ffd = open(target.fileSystemRepresentation,
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
                    if (!chunk) { if (ffd >= 0) close(ffd); return -1; }
                    uint64_t remaining = file_size;
                    BOOL ok = YES;
                    while (remaining > 0) {
                        size_t want = remaining > CLIP_FILE_CHUNK
                            ? CLIP_FILE_CHUNK : (size_t)remaining;
                        if (read_full(fd, chunk, want) <= 0) { ok = NO; break; }
                        if (ffd >= 0) {
                            const uint8_t *pp = chunk; size_t left = want;
                            while (left) {
                                ssize_t w = write(ffd, pp, left);
                                if (w <= 0) { ok = NO; break; }
                                pp += w; left -= (size_t)w;
                            }
                            if (!ok) break;
                        }
                        remaining -= want;
                    }
                    free(chunk);
                    if (ffd >= 0) close(ffd);
                    if (!ok) {
                        [p.cond lock];
                        p.failed = YES;
                        p.completed = YES;
                        [p.cond broadcast];
                        [p.cond unlock];
                        return -1;
                    }
                }
                /* Track top-level entries as pasteboard URLs. */
                if (relPath.pathComponents.count == 1) {
                    [p.cond lock];
                    if (!p.fileTreeUrls) p.fileTreeUrls = [NSMutableArray array];
                    [p.fileTreeUrls addObject:[NSURL fileURLWithPath:target]];
                    [p.cond unlock];
                }
                /* Countdown & complete. */
                [p.cond lock];
                p.fileCountExpected--;
                BOOL done = (p.fileCountExpected <= 0);
                if (done) {
                    p.completed = YES;
                    [p.cond broadcast];
                }
                [p.cond unlock];
                return 1;
            }

            /* Single-file fallback path (no tree dir). */
            [p.cond lock];
            p.fileSize = file_size;
            [p.cond unlock];
            uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
            if (!chunk) return -1;
            uint64_t remaining = file_size;
            BOOL ok = YES;
            while (remaining > 0) {
                size_t want = remaining > CLIP_FILE_CHUNK
                    ? CLIP_FILE_CHUNK : (size_t)remaining;
                if (read_full(fd, chunk, want) <= 0) { ok = NO; break; }
                [p.cond lock];
                if (p.fileFd >= 0) {
                    const uint8_t *pp = chunk; size_t left = want;
                    while (left) {
                        ssize_t w = write(p.fileFd, pp, left);
                        if (w <= 0) { ok = NO; break; }
                        pp += w; left -= (size_t)w;
                    }
                } else {
                    [p.body appendBytes:chunk length:want];
                }
                p.fileBytesWritten += want;
                [p.cond unlock];
                if (!ok) break;
                remaining -= want;
            }
            free(chunk);
            [p.cond lock];
            if (!ok) p.failed = YES;
            p.completed = YES;
            [p.cond broadcast];
            [p.cond unlock];
            return ok ? 1 : -1;
        }
    }

    /* Unknown — drain body. */
    if (h.data_size) {
        NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
        read_full(fd, trash.mutableBytes, h.data_size);
    }
    return 1;
}

/* ---- Pasteboard poll thread ---- */

static void *pb_poll_thread(void *arg) {
    (void)arg;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    g_last_seen_change = pb.changeCount;
    while (!g_stop) {
        usleep(250 * 1000);
        if (g_client_sock < 0) continue;
        NSInteger now = pb.changeCount;
        if (now == g_last_seen_change) continue;
        g_last_seen_change = now;

        NSInteger suppress = atomic_exchange(&g_suppress_change, -1);
        if (suppress == now) {
            clip_log(@"poll: suppressed echo at changeCount=%ld", (long)now);
            continue;
        }

        if (pb.pasteboardItems.count == 0) continue;
        send_format_list_v2(g_client_sock, pb);
    }
    return NULL;
}

/* ---- XPC: receive vsock fds from the root daemon ---- */

@interface AsbClipboardCallback : NSObject <AppSandboxClipboardCallback>
@end

static dispatch_queue_t g_session_queue;
static NSXPCConnection *g_xpc_conn;

@implementation AsbClipboardCallback

- (void)takeHostConnection:(NSFileHandle *)fh {
    /* NSFileHandle with closeOnDealloc=NO here — we take ownership of the
     * fd and close it ourselves when the session loop exits. */
    int fd = fh.fileDescriptor;
    if (fd < 0) {
        clip_log(@"takeHostConnection: bad fd");
        return;
    }
    /* Dup so NSFileHandle's eventual dealloc can't yank the fd from under
     * the session loop on the background queue. */
    int owned = dup(fd);
    if (owned < 0) {
        clip_log(@"takeHostConnection: dup failed: %s", strerror(errno));
        return;
    }
    clip_log(@"XPC: received host fd (dup=%d)", owned);
    dispatch_async(g_session_queue, ^{
        g_client_sock = owned;
        g_last_seen_change = NSPasteboard.generalPasteboard.changeCount;
        while (!g_stop) {
            if (handle_inbound(owned) <= 0) break;
        }
        clip_log(@"XPC: host session ended");
        pending_fail_all(@"host session ended");
        close(owned);
        g_client_sock = -1;
    });
}

@end

static void connect_to_daemon(void);

static void xpc_reconnect_after_delay(void) {
    if (g_stop) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        if (!g_stop) connect_to_daemon();
    });
}

static void connect_to_daemon(void) {
    NSXPCConnection *c = [[NSXPCConnection alloc]
        initWithMachServiceName:CLIPBOARD_XPC_MACH_SERVICE
                        options:NSXPCConnectionPrivileged];
    c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:
        @protocol(AppSandboxClipboardService)];
    c.exportedInterface = [NSXPCInterface interfaceWithProtocol:
        @protocol(AppSandboxClipboardCallback)];
    c.exportedObject = [AsbClipboardCallback new];

    c.invalidationHandler = ^{
        clip_log(@"XPC: connection invalidated, will retry");
        g_xpc_conn = nil;
        xpc_reconnect_after_delay();
    };
    c.interruptionHandler = ^{
        clip_log(@"XPC: connection interrupted");
    };
    [c resume];

    id<AppSandboxClipboardService> proxy =
        [c remoteObjectProxyWithErrorHandler:^(NSError *err) {
            clip_log(@"XPC: proxy error: %@", err.localizedDescription);
        }];
    [proxy helperReady];

    g_xpc_conn = c;
    clip_log(@"XPC: connected to %@", CLIPBOARD_XPC_MACH_SERVICE);
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_client_sock >= 0) { shutdown(g_client_sock, SHUT_RDWR); g_client_sock = -1; }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSURL *caches = [NSFileManager.defaultManager
            URLForDirectory:NSCachesDirectory
                   inDomain:NSUserDomainMask
          appropriateForURL:nil create:YES error:nil];
        g_cache_base = [[caches URLByAppendingPathComponent:@"com.appsandbox.clipboard"] path];
        [NSFileManager.defaultManager createDirectoryAtPath:g_cache_base
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];

        struct sigaction sa = {0};
        sa.sa_handler = on_signal;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT,  &sa, NULL);
        signal(SIGPIPE, SIG_IGN);

        g_session_queue = dispatch_queue_create(
            "com.appsandbox.clipboard.session", DISPATCH_QUEUE_SERIAL);

        pthread_t pollThread;
        pthread_create(&pollThread, NULL, pb_poll_thread, NULL);
        pthread_detach(pollThread);

        connect_to_daemon();

        /* Run loop keeps XPC delivery + dispatch-main callbacks alive. */
        while (!g_stop) {
            @autoreleasepool {
                [NSRunLoop.currentRunLoop
                    runMode:NSDefaultRunLoopMode
                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
            }
        }
    }
    return 0;
}
