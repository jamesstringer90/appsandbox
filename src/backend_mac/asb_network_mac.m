#import "asb_network_mac.h"
#include "asb_events.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static NSError *NetworkError(NSString *operation, NSInteger status) {
    return [NSError errorWithDomain:@"AppSandbox.Network" code:status
        userInfo:@{NSLocalizedDescriptionKey:
            [NSString stringWithFormat:@"%@: %ld", operation, (long)status]}];
}

static void NetworkLog(NSString *message) {
    CoreVmEvent event = { .type = CORE_VM_EVENT_LOG, .str_value = message.UTF8String };
    asb_events_emit(&event);
}

@implementation AsbNatNetwork {
    vmnet_network_ref _network;
    NSString *_subnet;
}
static AsbNatNetwork *g_network;

+ (instancetype)sharedNetworkWithError:(NSError **)error {
    @synchronized(self) {
        if (g_network) return g_network;
        vmnet_return_t status;
        vmnet_network_configuration_ref config =
            vmnet_network_configuration_create(VMNET_SHARED_MODE, &status);
        if (!config) {
            if (error) *error = NetworkError(@"Create NAT configuration", status);
            return nil;
        }
        vmnet_network_ref network = vmnet_network_create(config, &status);
        CFRelease(config);
        if (!network) {
            if (error) *error = NetworkError(@"Create shared NAT network", status);
            return nil;
        }
        AsbNatNetwork *owner = [[self alloc] init];
        owner->_network = network;
        struct in_addr subnet, mask;
        vmnet_network_get_ipv4_subnet(network, &subnet, &mask);
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &subnet, address, sizeof(address));
        owner->_subnet = [NSString stringWithFormat:@"%s/%d", address,
                         __builtin_popcount(ntohl(mask.s_addr))];
        g_network = owner;
        NetworkLog([NSString stringWithFormat:@"[NAT] Shared guest network %@", owner.subnet]);
        return owner;
    }
}
+ (void)reset { @synchronized(self) { g_network = nil; } }
- (vmnet_network_ref)network { return _network; }
- (NSString *)subnet { return _subnet; }
- (void)dealloc { if (_network) CFRelease(_network); }
@end

enum { kFrameLimit = 65536, kPendingLimit = 2 * 1024 * 1024, kBatch = 32 };
static char kPortQueue;

@interface AsbNatPort () {
    AsbNatNetwork *_owner;
    NSString *_name;
    dispatch_queue_t _queue;
    interface_ref _interface;
    BOOL _starting, _closed, _writeResumed, _datagram;
    int _fd;
    size_t _maxPacket;
    int _readBatch;
    NSFileHandle *_childHandle;
    dispatch_source_t _readSource, _writeSource;
    NSMutableData *_input, *_output, *_packets;
    NSUInteger _outputOffset;
    void (^_completion)(NSError *);
}
- (void)stopOnQueue;
@end

static void StopPort(void *context) { [(__bridge AsbNatPort *)context stopOnQueue]; }

@implementation AsbNatPort
- (instancetype)initWithNetwork:(AsbNatNetwork *)network name:(NSString *)name error:(NSError **)error {
    return [self initWithNetwork:network name:name datagram:NO error:error];
}

- (instancetype)initWithNetwork:(AsbNatNetwork *)network name:(NSString *)name
                      datagram:(BOOL)datagram error:(NSError **)error {
    if (!(self = [super init])) return nil;
    _fd = -1;
    _datagram = datagram;
    _owner = network;
    _name = [name copy];
    _queue = dispatch_queue_create("app.appsandbox.nat-port", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_queue, &kPortQueue, (__bridge void *)self, NULL);
    int sockets[2];
    if (socketpair(AF_UNIX, datagram ? SOCK_DGRAM : SOCK_STREAM, 0, sockets) != 0) {
        if (error) *error = NetworkError(@"Create guest network socket", errno);
        return nil;
    }
    _fd = sockets[0];
    _childHandle = [[NSFileHandle alloc] initWithFileDescriptor:sockets[1] closeOnDealloc:YES];
    if (datagram) {
        int sendSize = 64 * 1024, receiveSize = 256 * 1024;
        for (int i = 0; i < 2; i++) {
            if (setsockopt(sockets[i], SOL_SOCKET, SO_SNDBUF, &sendSize, sizeof(sendSize)) != 0 ||
                setsockopt(sockets[i], SOL_SOCKET, SO_RCVBUF, &receiveSize, sizeof(receiveSize)) != 0) {
                if (error) *error = NetworkError(@"Configure installer network buffers", errno);
                [self stop];
                return nil;
            }
        }
    }
    int one = 1;
    if (fcntl(_fd, F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(_fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(sockets[1], F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
        if (error) *error = NetworkError(@"Configure guest network socket", errno);
        [self stop];
        return nil;
    }
    _input = [NSMutableData data];
    _output = [NSMutableData data];
    return self;
}

- (void)finish:(NSError *)error {
    void (^completion)(NSError *) = _completion;
    _completion = nil;
    if (completion) dispatch_async(dispatch_get_main_queue(), ^{ completion(error); });
}

- (void)startWithCompletion:(void (^)(NSError *))completion {
    dispatch_async(_queue, ^{
        if (self->_closed || self->_starting || self->_interface) {
            dispatch_async(dispatch_get_main_queue(), ^{
                completion(NetworkError(@"Guest network port is unavailable", ECANCELED));
            });
            return;
        }
        self->_completion = [completion copy];
        self->_starting = YES;
        xpc_object_t desc = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_bool(desc, vmnet_allocate_mac_address_key, false);
        xpc_dictionary_set_bool(desc, vmnet_enable_isolation_key, false);
        self->_interface = vmnet_interface_start_with_network(self->_owner.network, desc, self->_queue,
            ^(vmnet_return_t status, xpc_object_t parameters) {
                self->_starting = NO;
                if (self->_closed) { [self releaseInterface]; return; }
                if (status != VMNET_SUCCESS) {
                    [self finish:NetworkError(@"Attach guest to shared NAT", status)];
                    [self stopOnQueue];
                    return;
                }
                self->_maxPacket = xpc_dictionary_get_uint64(parameters, vmnet_max_packet_size_key);
                uint64_t readLimit = xpc_dictionary_get_uint64(parameters, vmnet_read_max_packets_key);
                self->_readBatch = (int)MIN((uint64_t)kBatch, readLimit ? readLimit : 1);
                if (self->_maxPacket < 14 || self->_maxPacket > kFrameLimit) {
                    [self finish:NetworkError(@"Invalid NAT frame size", EINVAL)];
                    [self stopOnQueue];
                    return;
                }
                self->_packets = [NSMutableData dataWithLength:self->_maxPacket * kBatch];
                [self installSources];
                __weak AsbNatPort *weakPort = self;
                vmnet_return_t result = vmnet_interface_set_event_callback(self->_interface,
                    VMNET_INTERFACE_PACKETS_AVAILABLE, self->_queue,
                    ^(interface_event_t events, xpc_object_t event) {
                        (void)events; (void)event;
                        [weakPort readNetwork];
                    });
                if (result != VMNET_SUCCESS) {
                    [self finish:NetworkError(@"Watch guest NAT interface", result)];
                    [self stopOnQueue];
                    return;
                }
                [self finish:nil];
            });
        if (!self->_interface) {
            self->_starting = NO;
            [self finish:NetworkError(@"Start guest NAT interface", VMNET_FAILURE)];
            [self stopOnQueue];
        }
        __weak AsbNatPort *weakPort = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), self->_queue, ^{
            AsbNatPort *port = weakPort;
            if (port && port->_completion) {
                [port finish:NetworkError(@"Timed out attaching guest to NAT", ETIMEDOUT)];
                [port stopOnQueue];
            }
        });
    });
}

- (void)installSources {
    int fd = _fd;
    dispatch_group_t cancelled = dispatch_group_create();
    _readSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, _queue);
    _writeSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, fd, 0, _queue);
    __weak AsbNatPort *weakPort = self;
    dispatch_source_set_event_handler(_readSource, ^{ [weakPort readGuest]; });
    dispatch_source_set_event_handler(_writeSource, ^{ [weakPort flushOutput]; });
    dispatch_group_enter(cancelled);
    dispatch_group_enter(cancelled);
    dispatch_source_set_cancel_handler(_readSource, ^{ dispatch_group_leave(cancelled); });
    dispatch_source_set_cancel_handler(_writeSource, ^{ dispatch_group_leave(cancelled); });
    dispatch_group_notify(cancelled, _queue, ^{ close(fd); });
    dispatch_resume(_readSource);
}

- (void)flushOutput {
    if (_closed) return;
    while (_outputOffset < _output.length) {
        const char *bytes = (const char *)_output.bytes + _outputOffset;
        size_t length = _output.length - _outputOffset;
        if (_datagram) {
            uint32_t header;
            memcpy(&header, bytes, sizeof(header));
            length = ntohl(header);
            bytes += sizeof(header);
        }
        ssize_t sent = send(_fd, bytes, length, 0);
        if (sent > 0) {
            if (_datagram && (size_t)sent != length) {
                [self fail:NetworkError(@"Incomplete guest network datagram", EIO)]; return;
            }
            _outputOffset += (NSUInteger)sent + (_datagram ? 4 : 0);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        [self fail:NetworkError(@"Guest network socket disconnected", sent < 0 ? errno : EPIPE)];
        return;
    }
    BOOL pending = _outputOffset < _output.length;
    if (!pending) { _output.length = 0; _outputOffset = 0; }
    if (pending && !_writeResumed) { _writeResumed = YES; dispatch_resume(_writeSource); }
    if (!pending && _writeResumed) { _writeResumed = NO; dispatch_suspend(_writeSource); }
}

- (void)readNetwork {
    if (_closed || !_interface) return;
    for (int batch = 0; batch < 64; batch++) {
        struct iovec vectors[kBatch];
        struct vmpktdesc packets[kBatch];
        for (int i = 0; i < _readBatch; i++) {
            vectors[i] = (struct iovec){ (char *)_packets.mutableBytes + i * _maxPacket, _maxPacket };
            packets[i] = (struct vmpktdesc){ _maxPacket, &vectors[i], 1, 0 };
        }
        int count = _readBatch;
        vmnet_return_t status = vmnet_read(_interface, packets, &count);
        if (status != VMNET_SUCCESS) {
            NetworkLog([NSString stringWithFormat:@"[%@] NAT receive failed: %u", _name, status]);
            [self fail:NetworkError(@"Read shared NAT interface", status)]; return;
        }
        if (!count) return;
        if (_outputOffset) {
            [_output replaceBytesInRange:NSMakeRange(0, _outputOffset) withBytes:NULL length:0];
            _outputOffset = 0;
        }
        for (int i = 0; i < count; i++) {
            size_t length = packets[i].vm_pkt_size;
            if (length < 14 || length > _maxPacket || _output.length + length + 4 > kPendingLimit) continue;
            uint32_t header = htonl((uint32_t)length);
            [_output appendBytes:&header length:4];
            [_output appendBytes:vectors[i].iov_base length:length];
        }
        [self flushOutput];
        if (_closed || count < _readBatch) return;
    }
    __weak AsbNatPort *weakPort = self;
    dispatch_async(_queue, ^{ [weakPort readNetwork]; });
}

- (BOOL)writeFrame:(void *)bytes length:(size_t)length {
    if (length < 14 || length > _maxPacket) return YES;
    struct iovec vector = { bytes, length };
    struct vmpktdesc packet = { length, &vector, 1, 0 };
    int packets = 1;
    vmnet_return_t status = vmnet_write(_interface, &packet, &packets);
    if (status != VMNET_SUCCESS && status != VMNET_BUFFER_EXHAUSTED &&
        status != VMNET_PACKET_TOO_BIG && status != VMNET_INVALID_ARGUMENT) {
        NetworkLog([NSString stringWithFormat:@"[%@] NAT send failed: %u", _name, status]);
        [self fail:NetworkError(@"Write shared NAT interface", status)];
        return NO;
    }
    return YES;
}

- (void)readGuest {
    if (_closed) return;
    uint8_t buffer[kFrameLimit];
    for (int batch = 0; batch < 32; batch++) {
        struct iovec vector = { buffer, sizeof(buffer) };
        struct msghdr message = { .msg_iov = &vector, .msg_iovlen = 1 };
        ssize_t count = recvmsg(_fd, &message, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (count <= 0) { [self fail:NetworkError(@"Guest network socket disconnected", count < 0 ? errno : EPIPE)]; return; }
        if (_datagram) {
            if (message.msg_flags & MSG_TRUNC) {
                [self fail:NetworkError(@"Invalid guest network datagram", EMSGSIZE)]; return;
            }
            if (![self writeFrame:buffer length:(size_t)count]) return;
            continue;
        }
        [_input appendBytes:buffer length:(NSUInteger)count];
        NSUInteger offset = 0;
        while (_input.length - offset >= 4) {
            uint32_t header;
            memcpy(&header, (const char *)_input.bytes + offset, 4);
            size_t length = ntohl(header);
            if (length > kFrameLimit) { [self fail:NetworkError(@"Invalid guest network frame", EMSGSIZE)]; return; }
            if (_input.length - offset < length + 4) break;
            if (![self writeFrame:(char *)_input.mutableBytes + offset + 4 length:length]) return;
            offset += length + 4;
        }
        if (offset) [_input replaceBytesInRange:NSMakeRange(0, offset) withBytes:NULL length:0];
    }
}

- (BOOL)launchTask:(NSTask *)task error:(NSError **)error {
    __block BOOL launched = NO;
    __block NSError *launchError;
    dispatch_sync(_queue, ^{
        if (self->_closed || self->_starting || !self->_interface || !self->_childHandle) {
            launchError = NetworkError(@"Guest NAT interface is no longer ready", ENOTCONN);
            return;
        }
        task.standardInput = self->_childHandle;
        launched = [task launchAndReturnError:&launchError];
        [self->_childHandle closeFile]; self->_childHandle = nil;
    });
    if (!launched && error) *error = launchError;
    return launched;
}

- (void)fail:(NSError *)error {
    if (_closed) return;
    BOOL starting = _completion != nil;
    void (^failure)(NSError *) = self.onFailure;
    [self finish:error];
    [self stopOnQueue];
    if (!starting && failure) dispatch_async(dispatch_get_main_queue(), ^{ failure(error); });
}

- (void)releaseInterface {
    if (_interface && !_starting) {
        vmnet_interface_set_event_callback(_interface, VMNET_INTERFACE_PACKETS_AVAILABLE, NULL, NULL);
        NSString *name = _name;
        vmnet_return_t result = vmnet_stop_interface(_interface, _queue, ^(vmnet_return_t status) {
            if (status != VMNET_SUCCESS)
                NetworkLog([NSString stringWithFormat:@"[%@] NAT interface cleanup failed: %u", name, status]);
        });
        if (result != VMNET_SUCCESS)
            NetworkLog([NSString stringWithFormat:@"[%@] NAT interface cleanup was not scheduled: %u", name, result]);
        _interface = NULL;
        _owner = nil;
    }
}

- (void)stopOnQueue {
    if (_closed) return;
    _closed = YES;
    [self finish:NetworkError(@"Guest network startup cancelled", ECANCELED)];
    [_childHandle closeFile]; _childHandle = nil;
    if (_readSource) {
        if (!_writeResumed) dispatch_resume(_writeSource);
        dispatch_source_cancel(_writeSource);
        dispatch_source_cancel(_readSource);
        _readSource = nil; _writeSource = nil;
    } else if (_fd >= 0) {
        close(_fd);
    }
    _fd = -1;
    [self releaseInterface];
}
- (void)stop {
    if (!_queue) return;
    if (dispatch_get_specific(&kPortQueue) == (__bridge void *)self) [self stopOnQueue];
    else dispatch_sync_f(_queue, (__bridge void *)self, StopPort);
}
- (void)dealloc { [self stop]; }
@end
