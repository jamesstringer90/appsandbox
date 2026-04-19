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
#define CLIP_MSG_FORMAT_LIST        1u
#define CLIP_MSG_FORMAT_DATA_REQ    2u
#define CLIP_MSG_FORMAT_DATA_RESP   3u
#define CLIP_MSG_FILE_DATA          4u
#define CLIP_MAX_PAYLOAD            (64u * 1024u * 1024u)
#define CLIP_FILE_CHUNK             (1u * 1024u * 1024u)

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

/* ---- Instance ---- */

@interface VmClipboardMac () {
    int _sock;
    pthread_mutex_t _sendLock;
    NSInteger _lastChangeSeen;
    _Atomic NSInteger _suppressChange;
    dispatch_source_t _pbTimer;
}
@property (nonatomic, strong) VZVirtioSocketDevice *socketDevice;
@property (nonatomic, copy)   NSString *vmName;
@property (nonatomic, assign) BOOL running;
@property (nonatomic, strong) NSString *cacheBase;
- (void)logFmt:(NSString *)fmt, ...;
@end

@implementation VmClipboardMac

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device {
    if ((self = [super init])) {
        _vmName       = [vmName copy];
        _socketDevice = device;
        _sock         = -1;
        atomic_init(&_suppressChange, -1);
        pthread_mutex_init(&_sendLock, NULL);

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

    /* Accept/read loop on a background queue. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
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
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    NSInteger now = pb.changeCount;
    if (now == _lastChangeSeen) return;
    _lastChangeSeen = now;

    NSInteger suppress = atomic_exchange(&_suppressChange, -1);
    if (suppress == now) {
        [self logFmt:@"swallowed echo at changeCount=%ld", (long)now];
        return;
    }

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
        while (self.running) {
            if ([self handleInbound] <= 0) break;
        }
        _sock = -1;
        close(fd);
        if (self.running)
            [self logFmt:@"guest clipboard disconnected — will reconnect"];
    }
}

@end
