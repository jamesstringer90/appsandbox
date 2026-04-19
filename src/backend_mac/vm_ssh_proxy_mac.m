#import "vm_ssh_proxy_mac.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#define SSH_RELAY_BUF   8192
#define MAX_RELAYS      16

typedef struct {
    int tcp_fd;
    int vsock_fd;
    pthread_t thread;
    volatile int stop;
} SshRelay;

@interface VmSshProxyMac () {
    int         _listenSock;
    SshRelay    _relays[MAX_RELAYS];
    pthread_mutex_t _relaysLock;
}
@property (nonatomic, strong) VZVirtioSocketDevice *socketDevice;
@property (nonatomic, copy)   NSString *vmName;
@property (nonatomic, assign) int       port;
@property (nonatomic, assign) int       preferredPort;
@property (nonatomic, assign) BOOL      running;
@property (nonatomic, strong) dispatch_queue_t workQueue;
- (void)logFmt:(NSString *)fmt, ...;
@end

@implementation VmSshProxyMac

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device
                 initialPort:(int)preferredPort {
    if ((self = [super init])) {
        _vmName        = [vmName copy];
        _socketDevice  = device;
        _preferredPort = preferredPort;
        _listenSock    = -1;
        pthread_mutex_init(&_relaysLock, NULL);
        for (int i = 0; i < MAX_RELAYS; i++) {
            _relays[i].tcp_fd   = -1;
            _relays[i].vsock_fd = -1;
            _relays[i].stop     = 0;
        }
        _workQueue = dispatch_queue_create("com.appsandbox.ssh-proxy", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)dealloc {
    [self stop];
    pthread_mutex_destroy(&_relaysLock);
}

- (void)logFmt:(NSString *)fmt, ... {
    va_list ap; va_start(ap, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSLog(@"vm_ssh_proxy[%@]: %@", self.vmName, msg);
    VmSshProxyLog block = self.onLog;
    if (block) dispatch_async(dispatch_get_main_queue(), ^{ block(msg); });
}

#pragma mark - Lifecycle

- (void)start {
    if (self.running) return;
    self.running = YES;
    dispatch_async(self.workQueue, ^{ [self runAcceptLoop]; });
}

- (void)stop {
    if (!self.running) return;
    self.running = NO;

    int s = _listenSock;
    _listenSock = -1;
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }

    pthread_mutex_lock(&_relaysLock);
    for (int i = 0; i < MAX_RELAYS; i++) {
        _relays[i].stop = 1;
        if (_relays[i].tcp_fd   >= 0) { shutdown(_relays[i].tcp_fd,   SHUT_RDWR); close(_relays[i].tcp_fd);   _relays[i].tcp_fd   = -1; }
        if (_relays[i].vsock_fd >= 0) { shutdown(_relays[i].vsock_fd, SHUT_RDWR); close(_relays[i].vsock_fd); _relays[i].vsock_fd = -1; }
    }
    /* Detach rather than join — relay threads exit naturally on socket close. */
    for (int i = 0; i < MAX_RELAYS; i++) {
        if (_relays[i].thread) {
            pthread_detach(_relays[i].thread);
            _relays[i].thread = 0;
        }
    }
    pthread_mutex_unlock(&_relaysLock);
}

#pragma mark - Accept loop

- (int)bindLoopbackListener {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* Try the persisted port first. */
    if (self.preferredPort > 0) {
        a.sin_port = htons((uint16_t)self.preferredPort);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0) goto bound;
    }
    /* Fall back to ephemeral. */
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s); return -1;
    }
bound:
    if (listen(s, 4) != 0) { close(s); return -1; }

    socklen_t alen = sizeof(a);
    if (getsockname(s, (struct sockaddr *)&a, &alen) == 0) {
        self.port = ntohs(a.sin_port);
    }
    return s;
}

- (void)runAcceptLoop {
    _listenSock = [self bindLoopbackListener];
    if (_listenSock < 0) {
        [self logFmt:@"bind/listen failed"];
        self.running = NO;
        return;
    }

    [self logFmt:@"listening on 127.0.0.1:%d", self.port];
    if (self.onPortAssigned) {
        int p = self.port;
        dispatch_async(dispatch_get_main_queue(), ^{ self.onPortAssigned(p); });
    }

    while (self.running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(_listenSock, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(_listenSock + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        int client = accept(_listenSock, NULL, NULL);
        if (client < 0) continue;

        int vsock_fd = [self connectVsock];
        if (vsock_fd < 0) {
            [self logFmt:@"vsock connect failed"];
            close(client);
            continue;
        }

        [self logFmt:@"accepted TCP on 127.0.0.1:%d → guest vsock :%d",
            self.port, VM_SSH_PROXY_VSOCK_PORT];
        [self startRelayTcp:client vsock:vsock_fd];
    }

    if (_listenSock >= 0) { close(_listenSock); _listenSock = -1; }
}

#pragma mark - Vsock connect

- (int)connectVsock {
    if (!self.socketDevice) return -1;
    __block int fd = -1;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    /* VZ APIs must be called on main queue. */
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.socketDevice connectToPort:VM_SSH_PROXY_VSOCK_PORT
                        completionHandler:^(VZVirtioSocketConnection * _Nullable c,
                                            NSError * _Nullable err) {
            if (c && !err) fd = dup(c.fileDescriptor);
            dispatch_semaphore_signal(sem);
        }];
    });
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return fd;
}

#pragma mark - Relay management

- (int)allocateRelaySlot {
    for (int i = 0; i < MAX_RELAYS; i++) {
        if (_relays[i].tcp_fd < 0 && _relays[i].vsock_fd < 0) {
            return i;
        }
    }
    return -1;
}

- (void)startRelayTcp:(int)tcp_fd vsock:(int)vsock_fd {
    pthread_mutex_lock(&_relaysLock);
    int slot = [self allocateRelaySlot];
    if (slot < 0) {
        pthread_mutex_unlock(&_relaysLock);
        [self logFmt:@"max relays reached, dropping"];
        close(tcp_fd); close(vsock_fd);
        return;
    }
    _relays[slot].tcp_fd   = tcp_fd;
    _relays[slot].vsock_fd = vsock_fd;
    _relays[slot].stop     = 0;

    /* Thread arg is a pointer to our _relays[slot] — stays valid as long as
     * self is alive. The thread clears tcp_fd/vsock_fd before exiting. */
    pthread_create(&_relays[slot].thread, NULL, relay_main, &_relays[slot]);
    pthread_detach(_relays[slot].thread);
    pthread_mutex_unlock(&_relaysLock);
}

static void *relay_main(void *arg) {
    SshRelay *r = (SshRelay *)arg;
    char buf[SSH_RELAY_BUF];

    while (!r->stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(r->tcp_fd, &rfds);
        FD_SET(r->vsock_fd, &rfds);
        int maxfd = r->tcp_fd > r->vsock_fd ? r->tcp_fd : r->vsock_fd;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        if (FD_ISSET(r->tcp_fd, &rfds)) {
            ssize_t rd = recv(r->tcp_fd, buf, SSH_RELAY_BUF, 0);
            if (rd <= 0) break;
            if (send(r->vsock_fd, buf, rd, 0) != rd) break;
        }
        if (FD_ISSET(r->vsock_fd, &rfds)) {
            ssize_t rd = recv(r->vsock_fd, buf, SSH_RELAY_BUF, 0);
            if (rd <= 0) break;
            if (send(r->tcp_fd, buf, rd, 0) != rd) break;
        }
    }

    if (r->tcp_fd   >= 0) { close(r->tcp_fd);   r->tcp_fd   = -1; }
    if (r->vsock_fd >= 0) { close(r->vsock_fd); r->vsock_fd = -1; }
    r->thread = 0;
    return NULL;
}

@end
