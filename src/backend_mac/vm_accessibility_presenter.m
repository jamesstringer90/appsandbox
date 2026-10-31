#import "vm_accessibility_presenter.h"
#import "vm_accessibility_snapshot.h"
#import "../../tools/transport/accessibility_wire_mac.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

@implementation VmAccessibilityPresenter {
    NSTask *_task;
    int _fd;
    NSLock *_lock;
    dispatch_queue_t _writer;
    BOOL _stopped;
    BOOL _registered;
    NSAccessibilityRemoteUIElement *_root;
}
- (instancetype)init {
    if ((self = [super init])) {
        _fd = -1;
        _lock = [NSLock new];
        _writer = dispatch_queue_create("com.appsandbox.accessibility.remote.writer", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (NSAccessibilityRemoteUIElement *)root { return _root; }
- (void)dealloc { if (_fd >= 0) close(_fd); }
- (void)start {
    if (!NSClassFromString(@"NSAccessibilityRemoteUIElement")) { [self fail]; return; }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) { [self fail]; return; }
    _fd = sockets[0];
    fcntl(_fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct timeval timeout = { 1, 0 };
    setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    _task = [NSTask new];
    _task.executableURL = [NSBundle.mainBundle.bundleURL URLByAppendingPathComponent:@"Contents/Helpers/AppSandboxAccessibilityHost.app/Contents/MacOS/AppSandboxAccessibilityHost"];
    NSFileHandle *input = [[NSFileHandle alloc] initWithFileDescriptor:sockets[1] closeOnDealloc:YES];
    _task.standardInput = input;
    _task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
    _task.standardError = NSFileHandle.fileHandleWithNullDevice;
    BOOL launched = [_task launchAndReturnError:nil];
    [input closeFile];
    if (!launched) { [self fail]; return; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) {
            @autoreleasepool {
                NSDictionary *message = AsbAXReadMessage(self->_fd);
                if (!message) break;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (self->_stopped) return;
                    if ([message[@"type"] isEqual:@"ready"] && VmAccessibilityIsString(message[@"token"], 4096)) {
                        NSData *token = [[NSData alloc] initWithBase64EncodedString:message[@"token"] options:0];
                        if (!token.length) return;
                        [NSAccessibilityRemoteUIElement registerRemoteUIProcessIdentifier:self->_task.processIdentifier];
                        self->_registered = YES;
                        self->_root = [[NSAccessibilityRemoteUIElement alloc] initWithRemoteToken:token];
                    }
                    [self.delegate receiveRemoteMessage:message presenter:self];
                });
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{ [self fail]; });
    });
}
- (void)send:(NSDictionary *)message {
    dispatch_async(_writer, ^{
        [self->_lock lock];
        BOOL stopped = self->_stopped;
        [self->_lock unlock];
        if (!stopped && !AsbAXWriteMessage(self->_fd, message))
            dispatch_async(dispatch_get_main_queue(), ^{ [self fail]; });
    });
}
- (void)fail {
    if (_stopped) return;
    [self stop];
    [self.delegate presenterEnded:self];
}
- (void)stop {
    [_lock lock];
    _stopped = YES;
    if (_fd >= 0) shutdown(_fd, SHUT_RDWR);
    [_lock unlock];
    _root = nil;
    if (_registered) {
        [NSAccessibilityRemoteUIElement unregisterRemoteUIProcessIdentifier:_task.processIdentifier];
        _registered = NO;
    }
    if (_task.running) [_task terminate];
}
@end
