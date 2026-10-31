#import "vm_accessibility_connection.h"
#import "vm_accessibility_snapshot.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <float.h>

@interface VmAccessibilityGuestRequest : NSObject
@property dispatch_semaphore_t signal;
@property NSDictionary *response;
@property NSTimeInterval deadline;
@property (copy) NSString *session;
@end
@implementation VmAccessibilityGuestRequest
@end

@implementation VmAccessibilityConnection {
    int _fd;
    NSLock *_lock;
    NSLock *_writeLock;
    dispatch_queue_t _writer;
    NSMutableDictionary<NSString *, VmAccessibilityGuestRequest *> *_requests;
    BOOL _stopped;
    BOOL _deliveryPending;
    id _pendingState;
    NSTimeInterval _lastActivity;
    BOOL _supportsRefresh;
    BOOL _supportsNavigationQueries;
    BOOL _supportsActionConfirmation;
}

- (instancetype)initWithFileDescriptor:(int)fd {
    self = [super init];
    if (!self) return nil;
    _fd = dup(fd);
    if (_fd < 0) return nil;
    fcntl(_fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct timeval timeout = { 0, 200000 };
    setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    timeout = (struct timeval){ 0, 0 };
    setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    _lock = [NSLock new];
    _writeLock = [NSLock new];
    _requests = [NSMutableDictionary dictionary];
    _writer = dispatch_queue_create("com.appsandbox.accessibility.writer", DISPATCH_QUEUE_SERIAL);
    _lastActivity = VmAccessibilityNow();
    return self;
}

- (void)dealloc {
    if (_fd >= 0) close(_fd);
}

- (NSTimeInterval)lastActivity {
    [_lock lock];
    NSTimeInterval value = _lastActivity;
    [_lock unlock];
    return value;
}

- (void)stop {
    [_lock lock];
    if (!_stopped) {
        _stopped = YES;
        shutdown(_fd, SHUT_RDWR);
        for (VmAccessibilityGuestRequest *request in _requests.allValues) dispatch_semaphore_signal(request.signal);
        [_requests removeAllObjects];
        _pendingState = nil;
    }
    [_lock unlock];
}

- (BOOL)write:(NSDictionary *)message request:(NSString *)requestID {
    [_writeLock lock];
    [_lock lock];
    VmAccessibilityGuestRequest *request = requestID ? _requests[requestID] : nil;
    BOOL permitted = !_stopped && (!requestID || (request && request.deadline > VmAccessibilityNow()));
    [_lock unlock];
    BOOL result = permitted && AsbAXWriteMessage(_fd, message);
    [_writeLock unlock];
    if (permitted && !result) [self stop];
    return result;
}

- (void)offerState:(id)state {
    [_lock lock];
    if (_stopped) { [_lock unlock]; return; }
    BOOL ready = [state isKindOfClass:NSDictionary.class] && ([state[@"status"] isEqual:@"ready"] ||
        ([state[@"status"] isEqual:@"updating"] && ![state[@"invalidated"] boolValue]));
    if (!ready || ![_pendingState isKindOfClass:VmAccessibilitySnapshot.class] ||
        ![[(VmAccessibilitySnapshot *)_pendingState session] isEqual:state[@"session"]]) _pendingState = state;
    BOOL enqueue = !_deliveryPending;
    _deliveryPending = YES;
    [_lock unlock];
    if (enqueue) dispatch_async(dispatch_get_main_queue(), ^{
        [self->_lock lock];
        id pending = self->_pendingState;
        self->_pendingState = nil;
        self->_deliveryPending = NO;
        BOOL stopped = self->_stopped;
        [self->_lock unlock];
        if (!stopped && pending) [self.delegate receiveState:pending connection:self];
    });
}

- (void)begin {
    dispatch_async(_writer, ^{
        if (![self write:@{ @"type": @"subscribe", @"version": @(ASB_AX_VERSION), @"enabled": @YES }
                    request:nil]) [self stop];
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) {
            @autoreleasepool {
                NSDictionary *message = AsbAXReadMessage(self->_fd);
                if (![message isKindOfClass:NSDictionary.class]) break;
                NSString *type = message[@"type"];
                if (!VmAccessibilityIsString(type, 64)) break;
                [self->_lock lock];
                self->_lastActivity = VmAccessibilityNow();
                BOOL stopped = self->_stopped;
                [self->_lock unlock];
                if (stopped) break;
                if ([type isEqual:@"actionReady"]) {
                    NSString *requestID = message[@"requestId"], *session = message[@"session"];
                    if (!VmAccessibilityIsIdentifier(requestID) || !VmAccessibilityIsSessionIdentifier(session) ||
                        !VmAccessibilityIsInteger(message[@"version"], ASB_AX_VERSION, ASB_AX_VERSION)) break;
                    dispatch_async(self->_writer, ^{
                        [self->_lock lock];
                        VmAccessibilityGuestRequest *request = self->_requests[requestID];
                        NSTimeInterval remaining = [request.session isEqual:session] ?
                            MAX(0, request.deadline - VmAccessibilityNow()) : 0;
                        [self->_lock unlock];
                        [self write:@{@"type": @"actionCommit", @"version": @(ASB_AX_VERSION),
                            @"requestId": requestID, @"session": session,
                            @"remainingMs": @(MIN(ASB_AX_REQUEST_TIMEOUT_MS, remaining * 1000))} request:nil];
                    });
                } else if ([type isEqual:@"actionResult"]) {
                    NSString *requestID = message[@"requestId"];
                    if (!VmAccessibilityIsIdentifier(requestID) || !VmAccessibilityIsInteger(message[@"ok"], 0, 1)) break;
                    [self->_lock lock];
                    VmAccessibilityGuestRequest *request = self->_requests[requestID];
                    if (request) {
                        request.response = message;
                        [self->_requests removeObjectForKey:requestID];
                    }
                    [self->_lock unlock];
                    if (request) dispatch_semaphore_signal(request.signal);
                } else if ([type isEqual:@"snapshot"]) {
                    VmAccessibilitySnapshot *snapshot = VmAccessibilityParseSnapshot(message);
                    if (!snapshot) break;
                    [self offerState:snapshot];
                } else if ([type isEqual:@"status"]) {
                    if (!VmAccessibilityIsInteger(message[@"version"], ASB_AX_VERSION, ASB_AX_VERSION) ||
                        !VmAccessibilityIsSessionIdentifier(message[@"session"]) || !VmAccessibilityIsString(message[@"status"], 64) ||
                        (message[@"refreshSupported"] && !VmAccessibilityIsInteger(message[@"refreshSupported"], 0, 1)) ||
                        (message[@"navigationQueriesSupported"] && !VmAccessibilityIsInteger(message[@"navigationQueriesSupported"], 0, 1)) ||
                        (message[@"actionConfirmationSupported"] && !VmAccessibilityIsInteger(message[@"actionConfirmationSupported"], 0, 1)) ||
                        (message[@"invalidated"] && !VmAccessibilityIsInteger(message[@"invalidated"], 0, 1)) ||
                        ![@[@"permissionRequired", @"ready", @"inactive", @"updating", @"unavailable", @"captureFailed"] containsObject:message[@"status"]]) break;
                    [self->_lock lock];
                    self->_supportsRefresh = [message[@"refreshSupported"] boolValue];
                    self->_supportsNavigationQueries = [message[@"navigationQueriesSupported"] boolValue];
                    self->_supportsActionConfirmation = [message[@"actionConfirmationSupported"] boolValue];
                    [self->_lock unlock];
                    [self offerState:message];
                } else break;
            }
        }
        [self stop];
        dispatch_async(dispatch_get_main_queue(), ^{ [self.delegate connectionEnded:self]; });
    });
}

- (BOOL)supportsRefresh {
    [_lock lock];
    BOOL supported = _supportsRefresh;
    [_lock unlock];
    return supported;
}
- (BOOL)supportsNavigationQueries {
    [_lock lock];
    BOOL supported = _supportsNavigationQueries;
    [_lock unlock];
    return supported;
}

- (void)refresh:(uint64_t)generation {
    dispatch_async(_writer, ^{
        [self write:@{@"type": @"refresh", @"version": @(ASB_AX_VERSION), @"generation": @(generation)} request:nil];
    });
}

- (NSDictionary *)request:(NSDictionary *)message {
    NSString *requestID = NSUUID.UUID.UUIDString;
    NSMutableDictionary *outgoing = [message mutableCopy];
    outgoing[@"requestId"] = requestID;
    VmAccessibilityGuestRequest *request = [VmAccessibilityGuestRequest new];
    request.signal = dispatch_semaphore_create(0);
    NSTimeInterval now = VmAccessibilityNow();
    request.deadline = now + ASB_AX_REQUEST_TIMEOUT_MS / 1000.0;
    if (message[@"hostDeadline"]) {
        if (!VmAccessibilityIsNumber(message[@"hostDeadline"], 0, DBL_MAX)) return nil;
        request.deadline = MIN(request.deadline, [message[@"hostDeadline"] doubleValue]);
    }
    if (request.deadline <= now) return nil;
    request.session = message[@"session"];
    [outgoing removeObjectForKey:@"hostDeadline"];
    [_lock lock];
    if (_stopped || _requests.count >= ASB_AX_MAX_PENDING_REQUESTS) { [_lock unlock]; return nil; }
    outgoing[@"confirmBeforeExecution"] = @(_supportsActionConfirmation);
    _requests[requestID] = request;
    [_lock unlock];
    dispatch_async(_writer, ^{
        if (![self write:outgoing request:requestID]) {
            [self->_lock lock];
            [self->_requests removeObjectForKey:requestID];
            [self->_lock unlock];
            dispatch_semaphore_signal(request.signal);
        }
    });
    NSTimeInterval remaining = MAX(0, request.deadline - VmAccessibilityNow());
    dispatch_semaphore_wait(request.signal, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(remaining * NSEC_PER_SEC)));
    [_lock lock];
    [_requests removeObjectForKey:requestID];
    NSDictionary *response = request.response;
    [_lock unlock];
    return response;
}
@end
