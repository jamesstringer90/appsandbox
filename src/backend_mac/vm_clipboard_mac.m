#import "vm_clipboard_mac.h"
#import <AppKit/AppKit.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

/* ---- Wire protocol (mirrors appsandbox-clipboard.m exactly) ---- */

#define CLIP_MAGIC                  0x41434C50u

/* V1 (legacy eager) */
#define CLIP_MSG_FORMAT_LIST        1u
#define CLIP_MSG_FORMAT_DATA_REQ    2u
#define CLIP_MSG_FORMAT_DATA_RESP   3u
#define CLIP_MSG_FILE_DATA          4u

/* V2 (lazy deferred-rendering; seq_id is first 4 bytes of body) */
#define CLIP_MSG_FORMAT_LIST_V2      5u
#define CLIP_MSG_FORMAT_DATA_REQ_V2  6u
#define CLIP_MSG_FORMAT_DATA_RESP_V2 7u
#define CLIP_MSG_FILE_DATA_V2        8u
#define CLIP_MSG_FORMAT_DATA_CHUNK   9u
#define CLIP_MSG_FORMAT_DATA_END    10u
#define CLIP_MSG_SYNC_ENABLE        12u   /* body: 1 byte (0 or 1) */

#define CLIP_MAX_PAYLOAD            (256u * 1024u * 1024u)
#define CLIP_FILE_CHUNK             (1u * 1024u * 1024u)
#define CLIP_LARGE_THRESHOLD        (1u * 1024u * 1024u)

/* Toggle lazy (V2) host-side behavior. When YES, the poll thread emits
 * FORMAT_LIST_V2 and the V2 DATA_REQ_V2 path services pastes on demand.
 * Set to 0 to fall back to V1 eager transfer. */
#ifndef CLIP_V2_ENABLED
#define CLIP_V2_ENABLED 1
#endif

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

/* ---- Pending-request dispatcher (host receives from guest) ----
 * Mirror of the guest's pending table. Every outbound DATA_REQ_V2 carries
 * a seq; the receive loop routes the matching DATA_RESP_V2 / DATA_CHUNK /
 * DATA_END / FILE_DATA_V2 into the waiting provider thread's body/tree.
 *
 * Keyed on VmClipboardMac instance + seq so multiple VMs don't collide. */
@interface VmClipPending : NSObject
@property (atomic, assign)   uint32_t seqId;
@property (nonatomic, strong) NSCondition *cond;
@property (nonatomic, strong) NSMutableData *body;
@property (nonatomic, copy)   NSString *fileTreeDir;
@property (nonatomic, assign) int32_t fileCountExpected;
@property (nonatomic, strong) NSMutableArray<NSURL *> *fileTreeUrls;
@property (nonatomic, assign) BOOL completed;
@property (nonatomic, assign) BOOL failed;
@end
@implementation VmClipPending
- (instancetype)init {
    if ((self = [super init])) {
        _cond = [NSCondition new];
        _body = [NSMutableData data];
    }
    return self;
}
@end

/* Forward declaration — defined at the end of the file. Per-item
 * provider class; one instance per NSPasteboardItem we write. Each
 * knows its itemIndex on the guest side and serves all of that item's
 * types through blocking DATA_REQ_V2 round-trips. */
@class VmClipHostItemProvider;
@interface VmClipHostItemProvider : NSObject <NSPasteboardItemDataProvider>
@property (nonatomic, assign) uint64_t generation;
@property (nonatomic, assign) uint32_t itemIndex;
@property (nonatomic, strong) NSDictionary<NSString *, NSNumber *> *sizeHints;
@property (nonatomic, weak)   VmClipboardMac *clipboard;
@end

/* ---- Instance ---- */

@interface VmClipboardMac () {
    int _sock;
    pthread_mutex_t _sendLock;
    NSInteger _lastChangeSeen;
    _Atomic NSInteger _suppressChange;
    dispatch_source_t _pbTimer;
    pthread_mutex_t _pendingLock;
    NSMutableDictionary<NSNumber *, VmClipPending *> *_pending;
    _Atomic uint32_t _nextSeq;
    uint64_t _lazyGeneration;   /* bumped each time we overwrite the host pasteboard */
}
@property (nonatomic, strong) VZVirtioSocketDevice *socketDevice;
@property (nonatomic, copy)   NSString *vmName;
@property (nonatomic, assign) BOOL running;
@property (atomic,    assign) BOOL syncEnabled;
@property (nonatomic, strong) NSString *cacheBase;
- (void)logFmt:(NSString *)fmt, ...;
@end

@implementation VmClipboardMac
@synthesize syncEnabled = _syncEnabled;

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device {
    if ((self = [super init])) {
        _vmName       = [vmName copy];
        _socketDevice = device;
        _sock         = -1;
        atomic_init(&_suppressChange, -1);
        pthread_mutex_init(&_sendLock, NULL);
        pthread_mutex_init(&_pendingLock, NULL);
        _pending = [NSMutableDictionary dictionary];
        atomic_init(&_nextSeq, 1);
        _lazyGeneration = 1;

        NSURL *caches = [NSFileManager.defaultManager
            URLForDirectory:NSCachesDirectory inDomain:NSUserDomainMask
          appropriateForURL:nil create:YES error:nil];
        _cacheBase = [[caches URLByAppendingPathComponent:@"com.appsandbox.clipboard"] path];
        [NSFileManager.defaultManager createDirectoryAtPath:_cacheBase
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
    }
    return self;
}

- (void)dealloc {
    [self stop];
    pthread_mutex_destroy(&_sendLock);
    pthread_mutex_destroy(&_pendingLock);
}

#pragma mark - Pending dispatcher

- (uint32_t)allocSeq {
    uint32_t s = atomic_fetch_add(&_nextSeq, 1);
    if (s == 0) s = atomic_fetch_add(&_nextSeq, 1);
    return s;
}
- (void)pendingRegister:(VmClipPending *)p {
    pthread_mutex_lock(&_pendingLock);
    _pending[@(p.seqId)] = p;
    pthread_mutex_unlock(&_pendingLock);
}
- (VmClipPending *)pendingLookup:(uint32_t)seq {
    pthread_mutex_lock(&_pendingLock);
    VmClipPending *p = _pending[@(seq)];
    pthread_mutex_unlock(&_pendingLock);
    return p;
}
- (void)pendingRemove:(uint32_t)seq {
    pthread_mutex_lock(&_pendingLock);
    [_pending removeObjectForKey:@(seq)];
    pthread_mutex_unlock(&_pendingLock);
}
- (void)pendingFailAll {
    pthread_mutex_lock(&_pendingLock);
    NSArray *all = _pending.allValues;
    [_pending removeAllObjects];
    pthread_mutex_unlock(&_pendingLock);
    for (VmClipPending *p in all) {
        [p.cond lock];
        p.failed = YES;
        p.completed = YES;
        [p.cond broadcast];
        [p.cond unlock];
    }
}

- (void)logFmt:(NSString *)fmt, ... {
    va_list ap; va_start(ap, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSLog(@"vm_clipboard[%@]: %@", self.vmName, msg);
    VmClipboardLog block = self.onLog;
    if (block) dispatch_async(dispatch_get_main_queue(), ^{ block(msg); });
}

#pragma mark - Lifecycle

- (void)start {
    if (self.running) return;
    self.running = YES;

    /* The read loop fulfills V2 pending requests, which the pasteboard
     * data-provider callback on the main thread blocks on. Running the
     * reader at a lower QoS creates a priority inversion — the main
     * thread (User-interactive) waits on a lower-QoS thread. Use a
     * dedicated serial queue with explicit User-initiated QoS so the
     * level is guaranteed regardless of how the thread is scheduled. */
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    dispatch_queue_t readQueue = dispatch_queue_create(
        "com.appsandbox.clipboard.read", attr);
    dispatch_async(readQueue, ^{
        [self runReadLoop];
    });

    /* Pasteboard poll on main queue (NSPasteboard is main-queue safe). */
    __weak typeof(self) ws = self;
    _pbTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_main_queue());
    dispatch_source_set_timer(_pbTimer,
        dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
        250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(_pbTimer, ^{
        [ws pollPasteboard];
    });
    dispatch_resume(_pbTimer);
}

- (void)stop {
    if (!self.running) return;
    self.running = NO;

    if (_pbTimer) { dispatch_source_cancel(_pbTimer); _pbTimer = nil; }

    int s = _sock;
    _sock = -1;
    if (s >= 0) { shutdown(s, SHUT_RDWR); close(s); }
}

/* Paired getter needed because the atomic synthesis won't mix a
 * user-defined setter with a synthesized getter. Plain BOOL load is
 * atomic on Apple platforms. */
- (BOOL)syncEnabled {
    return _syncEnabled;
}

- (void)setSyncEnabled:(BOOL)enabled {
    BOOL prev = _syncEnabled;
    _syncEnabled = enabled;
    if (enabled && !prev) {
        /* Focus just arrived on a VM window — push the current host
         * clipboard now so the guest has it ready for a paste. Reset
         * the change tracker so the next poll tick treats it as new. */
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_lastChangeSeen = -1;
        });
    }
    /* Tell the guest so it mirrors the gate for guest→host syncing. */
    if (_sock >= 0) {
        uint8_t flag = enabled ? 1 : 0;
        [self sendMessage:CLIP_MSG_SYNC_ENABLE format:nil
                     data:[NSData dataWithBytes:&flag length:1]];
    }
    [self logFmt:@"clipboard sync %@", enabled ? @"enabled" : @"disabled"];
}

#pragma mark - Vsock connect

- (int)connectVsock {
    if (!self.socketDevice) return -1;
    __block int fd = -1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.socketDevice connectToPort:VM_CLIPBOARD_VSOCK_PORT
                        completionHandler:^(VZVirtioSocketConnection * _Nullable c,
                                            NSError * _Nullable err) {
            if (c && !err) fd = dup(c.fileDescriptor);
            dispatch_semaphore_signal(sem);
        }];
    });
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return fd;
}

#pragma mark - Host-side lazy pasteboard (guest → host)

- (void)writeLazyHostPasteboardItems:(NSArray<NSDictionary *> *)items {
    /* Called on main queue. Bump generation to orphan prior providers. */
    _lazyGeneration++;
    uint64_t gen = _lazyGeneration;

    NSMutableArray *writables = [NSMutableArray array];
    for (NSUInteger i = 0; i < items.count; i++) {
        NSDictionary *entry = items[i];
        NSArray<NSString *> *types = entry[@"types"];
        NSDictionary<NSString *, NSNumber *> *hints = entry[@"hints"];
        if (types.count == 0) continue;

        VmClipHostItemProvider *provider = [VmClipHostItemProvider new];
        provider.generation = gen;
        provider.itemIndex  = (uint32_t)i;
        provider.sizeHints  = hints;
        provider.clipboard  = self;

        NSPasteboardItem *pi = [NSPasteboardItem new];
        [pi setDataProvider:provider forTypes:types];
        [writables addObject:pi];
    }

    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb writeObjects:writables];
    atomic_store(&_suppressChange, pb.changeCount);
    [self logFmt:@"host lazy pasteboard written (gen=%llu, %u items, cc=%ld)",
        (unsigned long long)gen,
        (unsigned)writables.count, (long)pb.changeCount];
}

/* Send DATA_REQ_V2 with body = [seq, item_index] and uti in the header. */
- (int)sendDataReqV2:(uint32_t)seq
                item:(uint32_t)itemIndex
              format:(NSString *)uti {
    if (_sock < 0) return -1;
    NSData *fmt = uti ? [uti dataUsingEncoding:NSUTF8StringEncoding] : nil;
    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(CLIP_MSG_FORMAT_DATA_REQ_V2),
        .format_len = htonl((uint32_t)fmt.length),
        .data_size  = htonl(8),
    };
    uint32_t seqBE = htonl(seq);
    uint32_t idxBE = htonl(itemIndex);
    pthread_mutex_lock(&_sendLock);
    int rc = wr_full(_sock, &h, sizeof(h));
    if (rc > 0 && fmt.length) rc = wr_full(_sock, fmt.bytes, fmt.length);
    if (rc > 0) rc = wr_full(_sock, &seqBE, 4);
    if (rc > 0) rc = wr_full(_sock, &idxBE, 4);
    pthread_mutex_unlock(&_sendLock);
    return rc;
}

/* Fetch one item's typed data synchronously. */
- (nullable NSData *)fetchItemData:(uint32_t)itemIndex
                              type:(NSString *)type
                           timeout:(NSTimeInterval)timeout {
    if (_sock < 0) return nil;

    VmClipPending *p = [VmClipPending new];
    p.seqId = [self allocSeq];
    [self pendingRegister:p];

    if ([self sendDataReqV2:p.seqId item:itemIndex format:type] <= 0) {
        [self pendingRemove:p.seqId];
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
    [self pendingRemove:p.seqId];
    return result;
}

- (NSString *)allocHostTreeCacheDir {
    NSString *sub = [self.cacheBase stringByAppendingPathComponent:
                     NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtPath:sub
                            withIntermediateDirectories:YES
                                             attributes:nil error:nil];
    return sub;
}

/* Fetch one item's file subtree, return top-level URL. */
- (nullable NSURL *)fetchItemFileURL:(uint32_t)itemIndex
                             timeout:(NSTimeInterval)timeout {
    if (_sock < 0) return nil;

    VmClipPending *p = [VmClipPending new];
    p.seqId = [self allocSeq];
    p.fileTreeDir = [self allocHostTreeCacheDir];
    p.fileCountExpected = INT32_MAX;
    [self pendingRegister:p];

    if ([self sendDataReqV2:p.seqId item:itemIndex
                     format:NSPasteboardTypeFileURL] <= 0) {
        [self pendingRemove:p.seqId];
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
    [self pendingRemove:p.seqId];
    return urls.firstObject;
}

- (uint64_t)currentLazyGeneration {
    return _lazyGeneration;
}

#pragma mark - Low-level I/O

static int rd_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 0;
        p += r; n -= (size_t)r;
    }
    return 1;
}
static int wr_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 1;
}

- (int)sendMessage:(uint32_t)msgType
            format:(NSString *)formatUti
              data:(NSData *)data {
    if (_sock < 0) return -1;
    NSData *fmtData = formatUti
        ? [formatUti dataUsingEncoding:NSUTF8StringEncoding] : nil;
    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(msgType),
        .format_len = htonl((uint32_t)fmtData.length),
        .data_size  = htonl((uint32_t)data.length),
    };
    pthread_mutex_lock(&_sendLock);
    int rc = wr_full(_sock, &h, sizeof(h));
    if (rc > 0 && fmtData.length) rc = wr_full(_sock, fmtData.bytes, fmtData.length);
    if (rc > 0 && data.length)    rc = wr_full(_sock, data.bytes, data.length);
    pthread_mutex_unlock(&_sendLock);
    return rc;
}

/* V2 message: prefixes body with [seq_id (4 bytes BE)]. */
- (int)sendMessageV2:(uint32_t)msgType
                 seq:(uint32_t)seq
              format:(NSString *)formatUti
                data:(NSData *)data {
    if (_sock < 0) return -1;
    NSData *fmtData = formatUti
        ? [formatUti dataUsingEncoding:NSUTF8StringEncoding] : nil;
    uint32_t body = 4 + (uint32_t)data.length;
    ClipHeader h = {
        .magic      = htonl(CLIP_MAGIC),
        .msg_type   = htonl(msgType),
        .format_len = htonl((uint32_t)fmtData.length),
        .data_size  = htonl(body),
    };
    uint32_t seqBE = htonl(seq);
    pthread_mutex_lock(&_sendLock);
    int rc = wr_full(_sock, &h, sizeof(h));
    if (rc > 0 && fmtData.length) rc = wr_full(_sock, fmtData.bytes, fmtData.length);
    if (rc > 0)                   rc = wr_full(_sock, &seqBE, 4);
    if (rc > 0 && data.length)    rc = wr_full(_sock, data.bytes, data.length);
    pthread_mutex_unlock(&_sendLock);
    return rc;
}

#pragma mark - Pasteboard polling (outbound)

- (NSArray<NSString *> *)wantedFormats:(NSPasteboard *)pb {
    NSArray *want = @[ NSPasteboardTypeFileURL, NSPasteboardTypePNG,
                       NSPasteboardTypeTIFF, NSPasteboardTypeRTF,
                       NSPasteboardTypeHTML, NSPasteboardTypeString ];
    NSArray *actual = pb.types ?: @[];
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *t in want) if ([actual containsObject:t]) [out addObject:t];
    if ([out containsObject:NSPasteboardTypeFileURL]) {
        [out removeObject:NSPasteboardTypePNG];
        [out removeObject:NSPasteboardTypeTIFF];
    }
    return out;
}

- (void)pollPasteboard {
    if (_sock < 0) return;
    /* Only sync while a VM window is focused on the host. When unfocused
     * we don't push FORMAT_LIST_V2 to the guest at all, so system daemons
     * in the guest (Universal Clipboard, Spotlight, clipboard managers)
     * have no lazy items to probe and no fetches can be triggered. */
    if (!self.syncEnabled) return;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    NSInteger now = pb.changeCount;
    if (now == _lastChangeSeen) return;
    _lastChangeSeen = now;

    NSInteger suppress = atomic_exchange(&_suppressChange, -1);
    if (suppress == now) {
        [self logFmt:@"swallowed echo at changeCount=%ld", (long)now];
        return;
    }

#if CLIP_V2_ENABLED
    if (pb.pasteboardItems.count == 0) return;
    [self sendFormatListV2:pb];
#else
    NSArray<NSString *> *utis = [self wantedFormats:pb];
    if (utis.count == 0) return;
    NSMutableData *buf = [NSMutableData data];
    uint32_t count = htonl((uint32_t)utis.count);
    [buf appendBytes:&count length:4];
    for (NSString *uti in utis) {
        NSData *u = [uti dataUsingEncoding:NSUTF8StringEncoding];
        uint32_t len = htonl((uint32_t)u.length);
        [buf appendBytes:&len length:4];
        [buf appendData:u];
    }
    [self sendMessage:CLIP_MSG_FORMAT_LIST format:nil data:buf];
    [self logFmt:@"host → guest FORMAT_LIST [%@]",
        [utis componentsJoinedByString:@", "]];
#endif
}

#pragma mark - File tree (outbound)

/* Streaming FILE_DATA sender — matches Windows wire format:
 *   ClipHeader (data_size = sizeof(ClipFileInfo) + path_len)
 *   ClipFileInfo + path bytes
 *   file_size raw bytes streamed in CLIP_FILE_CHUNK pieces (not counted
 *   in data_size). Held under _sendLock for the whole message so the
 *   pasteboard-poll FORMAT_LIST can't interleave mid-stream. */
- (int)sendFileEntry:(NSString *)relPath isDir:(BOOL)isDir
            fileSize:(uint64_t)fileSize openFd:(int)openFd {
    if (_sock < 0) return -1;
    NSData *pathUTF8 = [relPath dataUsingEncoding:NSUTF8StringEncoding];
    uint32_t path_len  = (uint32_t)pathUTF8.length;
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
    pthread_mutex_lock(&_sendLock);
    if (rc > 0) rc = wr_full(_sock, &h, sizeof(h));
    if (rc > 0) rc = wr_full(_sock, &info, sizeof(info));
    if (rc > 0 && path_len) rc = wr_full(_sock, pathUTF8.bytes, path_len);

    if (rc > 0 && !isDir && fileSize > 0 && openFd >= 0) {
        uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
        if (!chunk) rc = -1;
        uint64_t remaining = fileSize;
        while (rc > 0 && remaining > 0) {
            size_t want = remaining > CLIP_FILE_CHUNK
                ? CLIP_FILE_CHUNK : (size_t)remaining;
            ssize_t got = read(openFd, chunk, want);
            if (got <= 0) { rc = -1; break; }
            if (wr_full(_sock, chunk, (size_t)got) <= 0) { rc = -1; break; }
            remaining -= (uint64_t)got;
        }
        if (chunk) free(chunk);
    }
    pthread_mutex_unlock(&_sendLock);
    return rc;
}

- (int)sendFileTree:(NSURL *)top {
    NSString *base = top.lastPathComponent;
    NSNumber *isDir = nil;
    [top getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (!isDir.boolValue) {
        int ffd = open(top.path.fileSystemRepresentation, O_RDONLY);
        uint64_t sz = 0;
        if (ffd >= 0) {
            struct stat st;
            if (fstat(ffd, &st) == 0) sz = (uint64_t)st.st_size;
        }
        int rc = [self sendFileEntry:base isDir:NO fileSize:sz openFd:ffd];
        if (ffd >= 0) close(ffd);
        return rc;
    }
    if ([self sendFileEntry:base isDir:YES fileSize:0 openFd:-1] <= 0) return -1;
    NSDirectoryEnumerator *en = [NSFileManager.defaultManager
        enumeratorAtURL:top
        includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                           options:0 errorHandler:nil];
    for (NSURL *child in en) {
        NSString *rel = [base stringByAppendingPathComponent:
                         [child.path substringFromIndex:top.path.length + 1]];
        NSNumber *cd = nil;
        [child getResourceValue:&cd forKey:NSURLIsDirectoryKey error:nil];
        if (cd.boolValue) {
            if ([self sendFileEntry:rel isDir:YES fileSize:0 openFd:-1] <= 0) return -1;
        } else {
            int ffd = open(child.path.fileSystemRepresentation, O_RDONLY);
            uint64_t sz = 0;
            if (ffd >= 0) {
                struct stat st;
                if (fstat(ffd, &st) == 0) sz = (uint64_t)st.st_size;
            }
            int rc = [self sendFileEntry:rel isDir:NO fileSize:sz openFd:ffd];
            if (ffd >= 0) close(ffd);
            if (rc <= 0) return -1;
        }
    }
    return 1;
}

#pragma mark - Data request handler (outbound response)

- (void)handleDataRequest:(NSString *)format {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [self logFmt:@"guest → host DATA_REQ %@", format];
    if ([format isEqualToString:NSPasteboardTypeFileURL]) {
        NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[NSURL.class]
                                                    options:@{}] ?: @[];
        NSMutableArray<NSURL *> *valid = [NSMutableArray array];
        for (NSURL *u in urls) if (u.isFileURL) [valid addObject:u];

        /* Count total entries (files + dirs) to send in the header payload. */
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
                for (NSURL *__unused c in en) total++;
            }
        }
        uint32_t be = htonl(total);
        NSData *count = [NSData dataWithBytes:&be length:4];
        [self sendMessage:CLIP_MSG_FORMAT_DATA_RESP format:format data:count];
        for (NSURL *u in valid) [self sendFileTree:u];
        return;
    }

    NSData *d = [pb dataForType:format];
    [self sendMessage:CLIP_MSG_FORMAT_DATA_RESP format:format data:(d ?: [NSData data])];
}

#pragma mark - V2 FORMAT_LIST

/* Filter one item's types down to what we transport. File-url shadows
 * png/tiff — Finder adds those as previews for a file copy. */
- (NSArray<NSString *> *)transportTypesForItem:(NSPasteboardItem *)item {
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

- (uint64_t)sizeHintForItem:(NSPasteboardItem *)item uti:(NSString *)uti {
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

- (NSData *)fileMetaForItem:(NSPasteboardItem *)item {
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

/* Send FORMAT_LIST_V2 per-item. Body:
 *   uint32 item_count
 *   per-item: uint32 type_count, N × (uti, size_hint, meta). */
- (int)sendFormatListV2:(NSPasteboard *)pb {
    NSArray<NSPasteboardItem *> *items = pb.pasteboardItems ?: @[];
    NSMutableData *itemsBody = [NSMutableData data];
    uint32_t itemCount = 0;

    for (NSPasteboardItem *item in items) {
        NSArray<NSString *> *utis = [self transportTypesForItem:item];
        if (utis.count == 0) continue;
        itemCount++;

        uint32_t tc = htonl((uint32_t)utis.count);
        [itemsBody appendBytes:&tc length:4];
        for (NSString *uti in utis) {
            NSData *u = [uti dataUsingEncoding:NSUTF8StringEncoding];
            uint32_t ulen = htonl((uint32_t)u.length);
            uint64_t hint = [self sizeHintForItem:item uti:uti];
            uint64_t hintBE = CFSwapInt64HostToBig(hint);
            NSData *meta = nil;
            if ([uti isEqualToString:NSPasteboardTypeFileURL]) {
                meta = [self fileMetaForItem:item];
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

    [self logFmt:@"host → guest FORMAT_LIST_V2 (%u items)", itemCount];
    return [self sendMessage:CLIP_MSG_FORMAT_LIST_V2 format:nil data:body];
}

#pragma mark - V2 outbound streaming

/* Chunked V2 DATA_RESP sender. For payloads ≤ CLIP_LARGE_THRESHOLD sends
 * one FORMAT_DATA_RESP_V2. For larger, sends FORMAT_DATA_RESP_V2 with no
 * body, a series of FORMAT_DATA_CHUNK messages, and FORMAT_DATA_END. All
 * messages carry the same seq. Checks cancel flag between chunks. */
- (int)sendDataRespV2:(uint32_t)seq format:(NSString *)fmt bytes:(NSData *)bytes {
    if ((uint32_t)bytes.length <= CLIP_LARGE_THRESHOLD) {
        return [self sendMessageV2:CLIP_MSG_FORMAT_DATA_RESP_V2
                               seq:seq format:fmt data:bytes];
    }
    /* Large: kick off with empty RESP_V2 then stream chunks. */
    int rc = [self sendMessageV2:CLIP_MSG_FORMAT_DATA_RESP_V2
                             seq:seq format:fmt data:nil];
    if (rc <= 0) return rc;

    const uint8_t *p = bytes.bytes;
    NSUInteger remaining = bytes.length;
    while (remaining > 0) {
        NSUInteger chunk = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : remaining;
        NSData *slice = [NSData dataWithBytesNoCopy:(void *)p length:chunk freeWhenDone:NO];
        rc = [self sendMessageV2:CLIP_MSG_FORMAT_DATA_CHUNK
                             seq:seq format:nil data:slice];
        if (rc <= 0) return rc;
        p += chunk; remaining -= chunk;
    }
    return [self sendMessageV2:CLIP_MSG_FORMAT_DATA_END seq:seq format:nil data:nil];
}

/* V2 file entry: same wire layout as V1 FILE_DATA (ClipFileInfo + path +
 * streamed bytes), but prefixed with seq_id under FILE_DATA_V2. The file
 * bytes after the header are NOT counted in data_size. Checks cancel. */
- (int)sendFileEntryV2:(uint32_t)seq relPath:(NSString *)relPath
                 isDir:(BOOL)isDir fileSize:(uint64_t)fileSize openFd:(int)openFd {
    if (_sock < 0) return -1;
    NSData *pathUTF8 = [relPath dataUsingEncoding:NSUTF8StringEncoding];
    uint32_t path_len  = (uint32_t)pathUTF8.length;
    /* data_size = seq(4) + sizeof(ClipFileInfo) + path_len */
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
    pthread_mutex_lock(&_sendLock);
    if (rc > 0) rc = wr_full(_sock, &h, sizeof(h));
    if (rc > 0) rc = wr_full(_sock, &seqBE, 4);
    if (rc > 0) rc = wr_full(_sock, &info, sizeof(info));
    if (rc > 0 && path_len) rc = wr_full(_sock, pathUTF8.bytes, path_len);

    if (rc > 0 && !isDir && fileSize > 0 && openFd >= 0) {
        uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
        if (!chunk) rc = -1;
        uint64_t remaining = fileSize;
        while (rc > 0 && remaining > 0) {
            size_t want = remaining > CLIP_FILE_CHUNK
                ? CLIP_FILE_CHUNK : (size_t)remaining;
            ssize_t got = read(openFd, chunk, want);
            if (got <= 0) { rc = -1; break; }
            if (wr_full(_sock, chunk, (size_t)got) <= 0) { rc = -1; break; }
            remaining -= (uint64_t)got;
        }
        if (chunk) free(chunk);
    }
    pthread_mutex_unlock(&_sendLock);
    return rc;
}

/* V2 per-file-tree sender: mirrors -sendFileTree: but using FILE_DATA_V2
 * and carrying seq. Walks one top-level URL and emits entries. */
- (int)sendFileTreeV2:(NSURL *)top seq:(uint32_t)seq {
    NSString *base = top.lastPathComponent;
    NSNumber *isDir = nil;
    [top getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (!isDir.boolValue) {
        int ffd = open(top.path.fileSystemRepresentation, O_RDONLY);
        uint64_t sz = 0;
        if (ffd >= 0) {
            struct stat st;
            if (fstat(ffd, &st) == 0) sz = (uint64_t)st.st_size;
        }
        int rc = [self sendFileEntryV2:seq relPath:base isDir:NO fileSize:sz openFd:ffd];
        if (ffd >= 0) close(ffd);
        return rc;
    }
    if ([self sendFileEntryV2:seq relPath:base isDir:YES fileSize:0 openFd:-1] <= 0) return -1;
    NSDirectoryEnumerator *en = [NSFileManager.defaultManager
        enumeratorAtURL:top
        includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                           options:0 errorHandler:nil];
    for (NSURL *child in en) {
        NSString *rel = [base stringByAppendingPathComponent:
                         [child.path substringFromIndex:top.path.length + 1]];
        NSNumber *cd = nil;
        [child getResourceValue:&cd forKey:NSURLIsDirectoryKey error:nil];
        if (cd.boolValue) {
            if ([self sendFileEntryV2:seq relPath:rel isDir:YES fileSize:0 openFd:-1] <= 0) return -1;
        } else {
            int ffd = open(child.path.fileSystemRepresentation, O_RDONLY);
            uint64_t sz = 0;
            if (ffd >= 0) {
                struct stat st;
                if (fstat(ffd, &st) == 0) sz = (uint64_t)st.st_size;
            }
            int rc = [self sendFileEntryV2:seq relPath:rel isDir:NO fileSize:sz openFd:ffd];
            if (ffd >= 0) close(ffd);
            if (rc <= 0) return -1;
        }
    }
    return 1;
}

- (void)handleDataRequestV2:(NSString *)format
                         seq:(uint32_t)seq
                        item:(uint32_t)itemIndex {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    NSArray<NSPasteboardItem *> *items = pb.pasteboardItems;
    [self logFmt:@"host → guest DATA_REQ_V2 seq=%u item=%u %@",
        seq, itemIndex, format];

    /* Intentionally NOT gated on syncEnabled. The gate only suppresses
     * proactive announcements (FORMAT_LIST_V2 from the host's poll). This
     * handler runs because something is actively pasting on the guest,
     * which is a user-initiated action — serve from the host pasteboard
     * regardless of focus. Without this, VM-to-VM paste-through-host
     * would break after a focus switch. */

    if (itemIndex >= items.count) {
        [self logFmt:@"DATA_REQ_V2 item %u out of range (count %lu)",
            itemIndex, (unsigned long)items.count];
        [self sendDataRespV2:seq format:format bytes:[NSData data]];
        return;
    }
    NSPasteboardItem *item = items[itemIndex];

    if ([format isEqualToString:NSPasteboardTypeFileURL]) {
        NSString *urlStr = [item stringForType:NSPasteboardTypeFileURL];
        NSURL *url = urlStr ? [NSURL URLWithString:urlStr] : nil;
        if (!url.isFileURL) {
            uint32_t zero = htonl(0);
            [self sendMessageV2:CLIP_MSG_FORMAT_DATA_RESP_V2
                            seq:seq format:format
                           data:[NSData dataWithBytes:&zero length:4]];
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
        [self sendMessageV2:CLIP_MSG_FORMAT_DATA_RESP_V2
                        seq:seq format:format
                       data:[NSData dataWithBytes:&be length:4]];
        [self sendFileTreeV2:url seq:seq];
        return;
    }

    NSData *d = [item dataForType:format];
    [self sendDataRespV2:seq format:format bytes:(d ?: [NSData data])];
}

#pragma mark - Inbound

- (NSString *)allocCacheDir {
    NSString *sub = [self.cacheBase stringByAppendingPathComponent:
                     NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtPath:sub
                            withIntermediateDirectories:YES
                                             attributes:nil error:nil];
    return sub;
}

/* Stream a FILE_DATA body:
 *   - header's data_size = sizeof(ClipFileInfo) + path_len
 *   - file_size raw bytes follow, read in CLIP_FILE_CHUNK pieces and
 *     written incrementally — never fully buffered in memory.
 * On success, if the entry is a top-level name (pathComponents.count == 1)
 * the materialized NSURL is returned via outTopLevel. */
- (BOOL)streamFileData:(ClipHeader)h
                destDir:(NSString *)destDir
            outTopLevel:(NSURL **)outTopLevel {
    if (h.data_size < sizeof(ClipFileInfo)) return NO;
    ClipFileInfo info;
    if (rd_full(_sock, &info, sizeof(info)) <= 0) return NO;
    uint32_t path_len  = ntohl(info.path_len);
    uint64_t file_size = CFSwapInt64BigToHost(info.file_size);
    uint32_t is_dir    = ntohl(info.is_directory);
    if ((uint64_t)sizeof(info) + path_len != h.data_size) return NO;

    char *pb = malloc((size_t)path_len + 1);
    if (!pb) return NO;
    if (path_len && rd_full(_sock, pb, path_len) <= 0) { free(pb); return NO; }
    pb[path_len] = 0;
    NSString *rel = [NSString stringWithUTF8String:pb];
    free(pb);

    if ([rel containsString:@".."]) {
        uint8_t drain[4096];
        uint64_t rem = file_size;
        while (rem > 0) {
            size_t want = rem > sizeof(drain) ? sizeof(drain) : (size_t)rem;
            if (rd_full(_sock, drain, want) <= 0) return NO;
            rem -= want;
        }
        return YES;
    }

    NSString *target = [destDir stringByAppendingPathComponent:rel];
    if (is_dir) {
        [NSFileManager.defaultManager createDirectoryAtPath:target
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        if (outTopLevel && rel.pathComponents.count == 1)
            *outTopLevel = [NSURL fileURLWithPath:target];
        return YES;
    }

    [NSFileManager.defaultManager createDirectoryAtPath:[target stringByDeletingLastPathComponent]
                            withIntermediateDirectories:YES
                                             attributes:nil error:nil];
    int ffd = open(target.fileSystemRepresentation,
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint8_t *chunk = malloc(CLIP_FILE_CHUNK);
    if (!chunk) { if (ffd >= 0) close(ffd); return NO; }
    uint64_t remaining = file_size;
    BOOL ok = YES;
    while (remaining > 0) {
        size_t want = remaining > CLIP_FILE_CHUNK
            ? CLIP_FILE_CHUNK : (size_t)remaining;
        if (rd_full(_sock, chunk, want) <= 0) { ok = NO; break; }
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
    if (outTopLevel && rel.pathComponents.count == 1)
        *outTopLevel = [NSURL fileURLWithPath:target];
    return YES;
}

- (void)writePasteboard:(NSDictionary<NSString *, NSData *> *)formats
               fileUrls:(NSArray<NSURL *> *)urls {
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pb = NSPasteboard.generalPasteboard;
        [pb clearContents];
        if (urls.count) [pb writeObjects:urls];
        for (NSString *fmt in formats) {
            if ([fmt isEqualToString:NSPasteboardTypeFileURL]) continue;
            NSData *d = formats[fmt];
            if (d.length) [pb setData:d forType:fmt];
        }
        atomic_store(&self->_suppressChange, pb.changeCount);
    });
}

- (int)handleInboundFormatList:(ClipHeader)h {
    NSMutableData *buf = [NSMutableData dataWithLength:h.data_size];
    if (rd_full(_sock, buf.mutableBytes, h.data_size) <= 0) return -1;
    const uint8_t *p = buf.bytes;
    const uint8_t *end = p + h.data_size;
    if (end - p < 4) return -1;
    uint32_t count; memcpy(&count, p, 4); p += 4; count = ntohl(count);

    NSMutableArray<NSString *> *utis = [NSMutableArray array];
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 4) return -1;
        uint32_t len; memcpy(&len, p, 4); p += 4; len = ntohl(len);
        if ((size_t)(end - p) < len) return -1;
        NSString *u = [[NSString alloc] initWithBytes:p length:len
                                             encoding:NSUTF8StringEncoding];
        if (u) [utis addObject:u];
        p += len;
    }

    NSMutableDictionary<NSString *, NSData *> *formats = [NSMutableDictionary dictionary];
    NSMutableArray<NSURL *> *fileUrls = [NSMutableArray array];
    for (NSString *fmt in utis) {
        if ([self sendMessage:CLIP_MSG_FORMAT_DATA_REQ format:fmt data:nil] <= 0) return -1;
        for (;;) {
            ClipHeader rh;
            if (rd_full(_sock, &rh, sizeof(rh)) <= 0) return -1;
            rh.magic = ntohl(rh.magic);
            rh.msg_type = ntohl(rh.msg_type);
            rh.format_len = ntohl(rh.format_len);
            rh.data_size = ntohl(rh.data_size);
            if (rh.magic != CLIP_MAGIC) return -1;
            if (rh.data_size > CLIP_MAX_PAYLOAD) return -1;

            NSString *respFmt = nil;
            if (rh.format_len) {
                char *fb = malloc(rh.format_len + 1);
                if (!fb) return -1;
                if (rd_full(_sock, fb, rh.format_len) <= 0) { free(fb); return -1; }
                fb[rh.format_len] = 0;
                respFmt = [NSString stringWithUTF8String:fb];
                free(fb);
            }

            if (rh.msg_type == CLIP_MSG_FORMAT_DATA_RESP) {
                if ([fmt isEqualToString:NSPasteboardTypeFileURL]) {
                    uint8_t cb[4] = {0};
                    if (rh.data_size >= 4) {
                        if (rd_full(_sock, cb, 4) <= 0) return -1;
                        if (rh.data_size > 4) {
                            NSMutableData *trail = [NSMutableData dataWithLength:rh.data_size - 4];
                            rd_full(_sock, trail.mutableBytes, rh.data_size - 4);
                        }
                    }
                    uint32_t n; memcpy(&n, cb, 4); n = ntohl(n);
                    NSString *dest = [self allocCacheDir];
                    for (uint32_t i = 0; i < n; i++) {
                        ClipHeader fh;
                        if (rd_full(_sock, &fh, sizeof(fh)) <= 0) return -1;
                        fh.magic = ntohl(fh.magic);
                        fh.msg_type = ntohl(fh.msg_type);
                        fh.format_len = ntohl(fh.format_len);
                        fh.data_size = ntohl(fh.data_size);
                        if (fh.magic != CLIP_MAGIC || fh.msg_type != CLIP_MSG_FILE_DATA)
                            return -1;
                        NSURL *topLevel = nil;
                        if (![self streamFileData:fh destDir:dest outTopLevel:&topLevel])
                            return -1;
                        if (topLevel) [fileUrls addObject:topLevel];
                    }
                    break;
                }
                NSMutableData *body = nil;
                if (rh.data_size) {
                    body = [NSMutableData dataWithLength:rh.data_size];
                    if (rd_full(_sock, body.mutableBytes, rh.data_size) <= 0) return -1;
                }
                formats[respFmt ?: fmt] = body ?: [NSData data];
                break;
            }
            if (rh.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:rh.data_size];
                rd_full(_sock, trash.mutableBytes, rh.data_size);
            }
        }
    }
    [self writePasteboard:formats fileUrls:fileUrls];
    [self logFmt:@"applied host pasteboard (%d formats, %d file URLs)",
        (int)formats.count, (int)fileUrls.count];
    return 1;
}

- (int)handleInbound {
    ClipHeader h;
    if (rd_full(_sock, &h, sizeof(h)) <= 0) return -1;
    h.magic = ntohl(h.magic);
    h.msg_type = ntohl(h.msg_type);
    h.format_len = ntohl(h.format_len);
    h.data_size = ntohl(h.data_size);
    if (h.magic != CLIP_MAGIC) return -1;
    if (h.data_size > CLIP_MAX_PAYLOAD) return -1;

    NSString *format = nil;
    if (h.format_len) {
        char *fb = malloc(h.format_len + 1);
        if (!fb) return -1;
        if (rd_full(_sock, fb, h.format_len) <= 0) { free(fb); return -1; }
        fb[h.format_len] = 0;
        format = [NSString stringWithUTF8String:fb];
        free(fb);
    }

    if (h.msg_type == CLIP_MSG_FORMAT_LIST) {
        return [self handleInboundFormatList:h];
    }
    if (h.msg_type == CLIP_MSG_FORMAT_DATA_REQ) {
        if (h.data_size) {
            NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
            rd_full(_sock, trash.mutableBytes, h.data_size);
        }
        if (format) [self handleDataRequest:format];
        return 1;
    }

    /* V2 request: body = seq (4) + item_index (4). */
    if (h.msg_type == CLIP_MSG_FORMAT_DATA_REQ_V2) {
        if (h.data_size < 8) {
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                rd_full(_sock, trash.mutableBytes, h.data_size);
            }
            return 1;
        }
        uint32_t seqBE = 0, idxBE = 0;
        if (rd_full(_sock, &seqBE, 4) <= 0) return -1;
        if (rd_full(_sock, &idxBE, 4) <= 0) return -1;
        uint32_t remaining = h.data_size - 8;
        if (remaining) {
            NSMutableData *trash = [NSMutableData dataWithLength:remaining];
            rd_full(_sock, trash.mutableBytes, remaining);
        }
        uint32_t seq = ntohl(seqBE);
        uint32_t idx = ntohl(idxBE);
        if (format) [self handleDataRequestV2:format seq:seq item:idx];
        return 1;
    }

    /* V2 from guest: FORMAT_LIST_V2 announces the guest pasteboard as a
     * per-item structure. Decode and write N lazy host NSPasteboardItems
     * mirroring the shape. */
    if (h.msg_type == CLIP_MSG_FORMAT_LIST_V2) {
        if (h.data_size < 4) {
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                rd_full(_sock, trash.mutableBytes, h.data_size);
            }
            return 1;
        }
        NSMutableData *buf = [NSMutableData dataWithLength:h.data_size];
        if (rd_full(_sock, buf.mutableBytes, h.data_size) <= 0) return -1;
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
                if (end - p < 4) goto host_parse_done;
                uint32_t ulen; memcpy(&ulen, p, 4); p += 4; ulen = ntohl(ulen);
                if ((size_t)(end - p) < ulen + 8 + 4) goto host_parse_done;
                NSString *uti = [[NSString alloc] initWithBytes:p length:ulen
                                                       encoding:NSUTF8StringEncoding];
                p += ulen;
                uint64_t hintBE; memcpy(&hintBE, p, 8); p += 8;
                uint64_t hint = CFSwapInt64BigToHost(hintBE);
                uint32_t mlen; memcpy(&mlen, p, 4); p += 4; mlen = ntohl(mlen);
                if ((size_t)(end - p) < mlen) goto host_parse_done;
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
    host_parse_done:

        [self logFmt:@"recv FORMAT_LIST_V2 from guest: %u items",
            (unsigned)items.count];
        if (items.count > 0) {
            NSArray *itemsCopy = [items copy];
            dispatch_async(dispatch_get_main_queue(), ^{
                [self writeLazyHostPasteboardItems:itemsCopy];
            });
        }
        return 1;
    }

    /* V2 responses from guest — route to pending waiter by seq. */
    if (h.msg_type == CLIP_MSG_FORMAT_DATA_RESP_V2 ||
        h.msg_type == CLIP_MSG_FORMAT_DATA_CHUNK   ||
        h.msg_type == CLIP_MSG_FORMAT_DATA_END     ||
        h.msg_type == CLIP_MSG_FILE_DATA_V2) {
        if (h.data_size < 4) {
            if (h.data_size) {
                NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
                rd_full(_sock, trash.mutableBytes, h.data_size);
            }
            return 1;
        }
        uint32_t seqBE = 0;
        if (rd_full(_sock, &seqBE, 4) <= 0) return -1;
        uint32_t seq = ntohl(seqBE);
        uint32_t remaining = h.data_size - 4;
        VmClipPending *p = [self pendingLookup:seq];
        if (!p) {
            if (remaining) {
                NSMutableData *trash = [NSMutableData dataWithLength:remaining];
                rd_full(_sock, trash.mutableBytes, remaining);
            }
            return 1;
        }

        if (h.msg_type == CLIP_MSG_FORMAT_DATA_RESP_V2) {
            if (p.fileTreeDir) {
                if (remaining < 4) {
                    if (remaining) {
                        NSMutableData *trash = [NSMutableData dataWithLength:remaining];
                        rd_full(_sock, trash.mutableBytes, remaining);
                    }
                    [p.cond lock];
                    p.failed = YES; p.completed = YES;
                    [p.cond broadcast];
                    [p.cond unlock];
                    return 1;
                }
                uint32_t cnt = 0;
                if (rd_full(_sock, &cnt, 4) <= 0) return -1;
                cnt = ntohl(cnt);
                if (remaining > 4) {
                    NSMutableData *trash = [NSMutableData dataWithLength:remaining - 4];
                    rd_full(_sock, trash.mutableBytes, remaining - 4);
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
            NSMutableData *body = nil;
            if (remaining) {
                body = [NSMutableData dataWithLength:remaining];
                if (rd_full(_sock, body.mutableBytes, remaining) <= 0) return -1;
            }
            [p.cond lock];
            if (body) [p.body appendData:body];
            p.completed = YES;
            [p.cond broadcast];
            [p.cond unlock];
            return 1;
        }

        if (h.msg_type == CLIP_MSG_FORMAT_DATA_CHUNK) {
            NSMutableData *body = nil;
            if (remaining) {
                body = [NSMutableData dataWithLength:remaining];
                if (rd_full(_sock, body.mutableBytes, remaining) <= 0) return -1;
            }
            [p.cond lock];
            if (body) [p.body appendData:body];
            [p.cond unlock];
            return 1;
        }

        if (h.msg_type == CLIP_MSG_FORMAT_DATA_END) {
            if (remaining) {
                NSMutableData *trash = [NSMutableData dataWithLength:remaining];
                rd_full(_sock, trash.mutableBytes, remaining);
            }
            [p.cond lock];
            p.completed = YES;
            [p.cond broadcast];
            [p.cond unlock];
            return 1;
        }

        if (h.msg_type == CLIP_MSG_FILE_DATA_V2) {
            if (remaining < sizeof(ClipFileInfo)) return -1;
            ClipFileInfo info;
            if (rd_full(_sock, &info, sizeof(info)) <= 0) return -1;
            uint32_t path_len  = ntohl(info.path_len);
            uint64_t file_size = CFSwapInt64BigToHost(info.file_size);
            uint32_t is_dir    = ntohl(info.is_directory);
            if ((uint64_t)sizeof(info) + path_len != remaining) return -1;

            char *pathBuf = malloc((size_t)path_len + 1);
            if (!pathBuf) return -1;
            if (path_len && rd_full(_sock, pathBuf, path_len) <= 0) {
                free(pathBuf); return -1;
            }
            pathBuf[path_len] = 0;
            NSString *relPath = [NSString stringWithUTF8String:pathBuf];
            free(pathBuf);

            if ([relPath containsString:@".."]) {
                uint64_t rem = file_size;
                uint8_t trash[4096];
                while (rem > 0) {
                    size_t w = rem > sizeof(trash) ? sizeof(trash) : (size_t)rem;
                    if (rd_full(_sock, trash, w) <= 0) return -1;
                    rem -= w;
                }
                return 1;
            }

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
                    uint64_t rem = file_size;
                    BOOL ok = YES;
                    while (rem > 0) {
                        size_t want = rem > CLIP_FILE_CHUNK
                            ? CLIP_FILE_CHUNK : (size_t)rem;
                        if (rd_full(_sock, chunk, want) <= 0) { ok = NO; break; }
                        if (ffd >= 0) {
                            const uint8_t *pp = chunk; size_t left = want;
                            while (left) {
                                ssize_t w = write(ffd, pp, left);
                                if (w <= 0) { ok = NO; break; }
                                pp += w; left -= (size_t)w;
                            }
                            if (!ok) break;
                        }
                        rem -= want;
                    }
                    free(chunk);
                    if (ffd >= 0) close(ffd);
                    if (!ok) {
                        [p.cond lock];
                        p.failed = YES; p.completed = YES;
                        [p.cond broadcast];
                        [p.cond unlock];
                        return -1;
                    }
                }
                if (relPath.pathComponents.count == 1) {
                    [p.cond lock];
                    if (!p.fileTreeUrls) p.fileTreeUrls = [NSMutableArray array];
                    [p.fileTreeUrls addObject:[NSURL fileURLWithPath:target]];
                    [p.cond unlock];
                }
                [p.cond lock];
                p.fileCountExpected--;
                BOOL done = p.fileCountExpected <= 0;
                if (done) {
                    p.completed = YES;
                    [p.cond broadcast];
                }
                [p.cond unlock];
                return 1;
            }
            /* No tree dir — unexpected, just drain. */
            uint64_t rem = file_size;
            uint8_t trash[4096];
            while (rem > 0) {
                size_t w = rem > sizeof(trash) ? sizeof(trash) : (size_t)rem;
                if (rd_full(_sock, trash, w) <= 0) return -1;
                rem -= w;
            }
            return 1;
        }
    }

    if (h.data_size) {
        NSMutableData *trash = [NSMutableData dataWithLength:h.data_size];
        rd_full(_sock, trash.mutableBytes, h.data_size);
    }
    return 1;
}

- (void)runReadLoop {
    while (self.running) {
        int fd = [self connectVsock];
        if (fd < 0) {
            for (int i = 0; i < 30 && self.running; i++) usleep(100 * 1000);
            continue;
        }
        _sock = fd;
        _lastChangeSeen = NSPasteboard.generalPasteboard.changeCount;
        [self logFmt:@"connected to guest clipboard (vsock :%d)",
            VM_CLIPBOARD_VSOCK_PORT];
        /* Tell the guest our current focus state on every connect so a
         * freshly-booted or reconnecting helper starts in the right mode. */
        {
            uint8_t flag = self.syncEnabled ? 1 : 0;
            [self sendMessage:CLIP_MSG_SYNC_ENABLE format:nil
                         data:[NSData dataWithBytes:&flag length:1]];
        }
        while (self.running) {
            if ([self handleInbound] <= 0) break;
        }
        _sock = -1;
        close(fd);
        [self pendingFailAll];
        if (self.running)
            [self logFmt:@"guest clipboard disconnected — will reconnect"];
    }
}

@end

#pragma mark - Host-side per-item lazy provider

@implementation VmClipHostItemProvider {
    NSMutableDictionary<NSString *, NSData *> *_cache;
    NSLock *_cacheLock;
}

- (instancetype)init {
    if ((self = [super init])) {
        _cache = [NSMutableDictionary dictionary];
        _cacheLock = [NSLock new];
    }
    return self;
}

- (void)pasteboard:(NSPasteboard *)pb item:(NSPasteboardItem *)item
provideDataForType:(NSString *)type {
    VmClipboardMac *clip = self.clipboard;
    if (!clip || self.generation != [clip currentLazyGeneration]) return;

    [_cacheLock lock];
    NSData *cached = _cache[type];
    [_cacheLock unlock];
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
        NSURL *url = [clip fetchItemFileURL:self.itemIndex timeout:ftimeout];
        if (!url) return;
        result = [url.absoluteString dataUsingEncoding:NSUTF8StringEncoding];
    } else {
        NSTimeInterval timeout = hint > 10ULL * 1024 * 1024 ? 60.0
                               : hint > 1ULL  * 1024 * 1024 ? 10.0
                               : 2.0;
        result = [clip fetchItemData:self.itemIndex type:type timeout:timeout];
    }

    if (!result) return;
    [_cacheLock lock];
    _cache[type] = result;
    [_cacheLock unlock];
    [item setData:result forType:type];
}

@end
