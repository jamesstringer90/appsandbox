#import "vm_agent_mac.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

#define RECONNECT_BACKOFF_SEC   3
#define RECV_TIMEOUT_SEC        5

@interface VmAgentMac () {
    int                         _sock;
    unsigned int                _cmdSeq;
    dispatch_queue_t            _queue;
    NSCondition                *_cmdCond;
    NSString                   *_cmdPending;
    NSString                   *_cmdResponse;
    BOOL                        _cmdReady;
}
@property (nonatomic, strong) VZVirtioSocketDevice *socketDevice;
@property (nonatomic, copy)   NSString *vmName;
@property (nonatomic, assign) BOOL online;
@property (nonatomic, assign) uint64_t lastHeartbeatMs;
@property (nonatomic, assign) BOOL stopRequested;
@property (nonatomic, assign) BOOL running;
@end

@implementation VmAgentMac

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device {
    if ((self = [super init])) {
        _vmName = [vmName copy];
        _socketDevice = device;
        _sock = -1;
        _cmdSeq = 0;
        _queue = dispatch_queue_create("com.appsandbox.agent", DISPATCH_QUEUE_SERIAL);
        _cmdCond = [[NSCondition alloc] init];
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

#pragma mark - Lifecycle

- (void)start {
    if (self.running) return;
    self.running = YES;
    self.stopRequested = NO;
    dispatch_async(_queue, ^{ [self runLoop]; });
}

- (void)stop {
    self.stopRequested = YES;
    [self closeSocket];
    [_cmdCond lock];
    _cmdReady = YES;
    [_cmdCond broadcast];
    [_cmdCond unlock];
    if (self.online) {
        [self setOnlineOnMain:NO];
    }
    self.running = NO;
}

- (void)closeSocket {
    int s;
    @synchronized (self) {
        s = _sock;
        _sock = -1;
    }
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }
}

#pragma mark - Main connect/read loop

- (void)runLoop {
    while (!self.stopRequested) {
        int fd = [self connectOnce];
        if (fd < 0) {
            for (int i = 0; i < RECONNECT_BACKOFF_SEC * 10 && !self.stopRequested; i++) {
                usleep(100000);
            }
            continue;
        }

        @synchronized (self) { _sock = fd; }

        struct timeval tv = { .tv_sec = RECV_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char buf[512];
        int n = [self recvLine:fd into:buf size:sizeof(buf)];
        if (n <= 0 || strcmp(buf, "hello") != 0) {
            [self closeSocket];
            continue;
        }

        [self logFmt:@"Agent online for \"%@\".", self.vmName];
        self.lastHeartbeatMs = [self nowMs];
        [self setOnlineOnMain:YES];

        /* Request SSH bring-up if configured. Matches Windows vm_agent.c. */
        if (self.sshEnabled) {
            NSString *prefix = [NSString stringWithFormat:@"%u:", ++_cmdSeq];
            NSString *tagged = [NSString stringWithFormat:@"%@ssh_enable", prefix];
            if ([self sendLine:fd string:tagged] > 0) {
                NSString *resp = [self readTaggedResponse:fd prefix:prefix];
                if (resp) {
                    [self processSshStateReply:resp];
                } else {
                    /* Connection dropped while waiting. Let the outer read loop
                     * handle reconnect. */
                    [self closeSocket];
                    [self setOnlineOnMain:NO];
                    continue;
                }
            }
        }

        while (!self.stopRequested) {
            /* Dispatch pending command if any. */
            NSString *cmd = nil;
            [_cmdCond lock];
            if (_cmdPending) {
                cmd = _cmdPending;
                _cmdPending = nil;
            }
            [_cmdCond unlock];

            if (cmd) {
                _cmdSeq++;
                NSString *tagged = [NSString stringWithFormat:@"%u:%@", _cmdSeq, cmd];
                if ([self sendLine:fd string:tagged] <= 0) {
                    [self deliverResponse:nil];
                    break;
                }
                NSString *prefix = [NSString stringWithFormat:@"%u:", _cmdSeq];
                NSString *resp = [self readTaggedResponse:fd prefix:prefix];
                if (!resp) {
                    [self deliverResponse:nil];
                    break;
                }
                [self deliverResponse:resp];
                continue;
            }

            /* Wait for next line with short timeout to allow cmd pickup. */
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval to = { .tv_sec = 0, .tv_usec = 200000 };
            int ret = select(fd + 1, &rfds, NULL, NULL, &to);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;

            n = [self recvLine:fd into:buf size:sizeof(buf)];
            if (n <= 0) break;
            [self processAsyncMessage:buf];
        }

        [self logFmt:@"Agent offline for \"%@\".", self.vmName];
        [self closeSocket];
        [self setOnlineOnMain:NO];
        [self deliverResponse:nil];
    }
}

- (int)connectOnce {
    if (!self.socketDevice) return -1;

    __block int fd = -1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    /* VZVirtioSocketDevice.connectToPort: must be called on the VM's
     * queue (main queue for us). The completion handler also fires on
     * main. Agent loop runs on a serial queue, so hop to main for the
     * call and block on the semaphore. */
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.socketDevice connectToPort:VM_AGENT_MAC_PORT
                        completionHandler:^(VZVirtioSocketConnection * _Nullable c,
                                            NSError * _Nullable err) {
            if (c && !err) {
                /* Dup so the connection object's dealloc doesn't close our fd. */
                fd = dup(c.fileDescriptor);
            }
            dispatch_semaphore_signal(sem);
        }];
    });

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    return fd;
}

#pragma mark - Protocol

- (NSString *)readTaggedResponse:(int)fd prefix:(NSString *)prefix {
    const char *pfx = prefix.UTF8String;
    size_t pfxLen = strlen(pfx);
    char buf[512];
    while (!self.stopRequested) {
        int n = [self recvLine:fd into:buf size:sizeof(buf)];
        if (n <= 0) return nil;
        if (strncmp(buf, pfx, pfxLen) == 0) {
            return [NSString stringWithUTF8String:buf + pfxLen];
        }
        [self processAsyncMessage:buf];
    }
    return nil;
}

- (void)processSshStateReply:(NSString *)reply {
    int state = 0;
    if ([reply isEqualToString:@"ssh_ready"])       state = 2;
    else if ([reply isEqualToString:@"ssh_installing"]) state = 1;
    else if ([reply isEqualToString:@"ssh_failed"])     state = 3;
    else return;

    [self logFmt:@"[%@] SSH: %@", self.vmName, reply];
    VmAgentSshStateChange cb = self.onSshStateChange;
    if (cb) {
        dispatch_async(dispatch_get_main_queue(), ^{ cb(state); });
    }
}

- (void)processAsyncMessage:(const char *)line {
    if (strcmp(line, "heartbeat") == 0) {
        self.lastHeartbeatMs = [self nowMs];
        return;
    }
    if (strcmp(line, "os_shutdown") == 0) {
        [self logFmt:@"Guest OS shutting down for \"%@\".", self.vmName];
        return;
    }
    if (strcmp(line, "service_stopping") == 0) {
        [self logFmt:@"Agent service stopping in \"%@\".", self.vmName];
        return;
    }
    if (strncmp(line, "log:", 4) == 0) {
        [self logFmt:@"[%@] %s", self.vmName, line + 4];
        return;
    }
    if (strncmp(line, "ip:", 3) == 0) {
        /* Format: "ip:<iface>:<addr>" — reported by the agent on connect
         * so the user can SSH into the VM without guessing. */
        [self logFmt:@"[%@] IP %s", self.vmName, line + 3];
        return;
    }
    if (strcmp(line, "ssh_ready") == 0 ||
        strcmp(line, "ssh_installing") == 0 ||
        strcmp(line, "ssh_failed") == 0) {
        [self processSshStateReply:[NSString stringWithUTF8String:line]];
        return;
    }
    [self logFmt:@"[%@] (async) %s", self.vmName, line];
}

#pragma mark - Command dispatch

- (NSString *)sendCommand:(NSString *)cmd timeout:(NSTimeInterval)timeoutSec {
    if (!self.online) return nil;

    [_cmdCond lock];
    _cmdPending = [cmd copy];
    _cmdResponse = nil;
    _cmdReady = NO;
    [_cmdCond unlock];

    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSec];
    NSString *resp = nil;
    [_cmdCond lock];
    while (!_cmdReady) {
        if (![_cmdCond waitUntilDate:deadline]) break;
    }
    resp = _cmdResponse;
    _cmdResponse = nil;
    [_cmdCond unlock];
    return resp;
}

- (void)deliverResponse:(nullable NSString *)resp {
    [_cmdCond lock];
    _cmdResponse = resp;
    _cmdReady = YES;
    [_cmdCond broadcast];
    [_cmdCond unlock];
}

#pragma mark - Line I/O

- (int)sendLine:(int)fd string:(NSString *)s {
    const char *c = s.UTF8String;
    size_t len = strlen(c);
    ssize_t n = send(fd, c, len, 0);
    if (n <= 0) return (int)n;
    return (int)send(fd, "\n", 1, 0);
}

- (int)recvLine:(int)fd into:(char *)buf size:(int)size {
    int pos = 0;
    while (pos < size - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return (int)n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

#pragma mark - Helpers

- (uint64_t)nowMs {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

- (void)setOnlineOnMain:(BOOL)online {
    if (self.online == online) return;
    self.online = online;
    VmAgentOnlineChange block = self.onOnlineChange;
    if (block) {
        dispatch_async(dispatch_get_main_queue(), ^{ block(online); });
    }
}

- (void)logFmt:(NSString *)fmt, ... {
    va_list ap;
    va_start(ap, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    VmAgentLog block = self.onLog;
    if (block) {
        dispatch_async(dispatch_get_main_queue(), ^{ block(msg); });
    }
}

@end
