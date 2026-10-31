#import "accessibility_element.h"
#import "../../src/backend_mac/vm_accessibility_appkit.h"
#import "../../src/backend_mac/vm_accessibility_snapshot.h"
#import "../../tools/transport/accessibility_value_mac.h"
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>

static id VmAccessibilityRecordValue(NSDictionary *record) {
    return record[@"attributes"][@"AXValue"] ?: record[@"value"] ?: NSNull.null;
}

@interface VmAccessibilityClientRequest : NSObject
@property dispatch_semaphore_t signal;
@property NSDictionary *response;
@end
@implementation VmAccessibilityClientRequest
@end

@interface VmAccessibilityRemoteElement : VmAccessibilityElement
@property (readonly) NSRect accessibilityFrameInParentSpace;
@end

@interface VmAccessibilityProvider : NSObject <VmAccessibilityElementOwner>
@property (readonly) id window;
- (BOOL)send:(NSDictionary *)message;
- (instancetype)initWithFileDescriptor:(int)fd;
- (void)begin;
- (NSDictionary *)stateWaiting:(BOOL)wait;
- (id)hitTest:(NSPoint)point;
@end

@interface VmAccessibilityApplication : NSApplication
@property (weak) VmAccessibilityProvider *accessibilityServer;
@end

@implementation VmAccessibilityApplication
- (void)sendEvent:(NSEvent *)event {
    if (VmAccessibilityIsInputEvent(event) && self.accessibilityServer && event.CGEvent) {
        NSData *data = CFBridgingRelease(CGEventCreateData(NULL, event.CGEvent));
        if (data.length && [self.accessibilityServer send:@{@"type": @"input", @"event": [data base64EncodedStringWithOptions:0]}]) return;
    }
    [super sendEvent:event];
}
@end

@implementation VmAccessibilityRemoteElement
- (pid_t)accessibilityPresenterProcessIdentifier { return getppid(); }
- (id)hitTestGuest:(NSPoint)point {
    VmAccessibilityProvider *server = (VmAccessibilityProvider *)self.owner;
    return [[server stateWaiting:NO][@"navigationQueries"] boolValue] ? [server hitTest:point] : [super hitTestGuest:point];
}
- (NSRect)accessibilityFrameInParentSpace {
    NSRect frame = self.accessibilityFrame;
    id parent = self.accessibilityParent;
    NSRect parentFrame = [parent isKindOfClass:VmAccessibilityElement.class] ? [parent accessibilityFrame] :
        [[(VmAccessibilityProvider *)self.owner stateWaiting:NO][@"frame"] rectValue];
    frame.origin.x -= parentFrame.origin.x;
    frame.origin.y -= parentFrame.origin.y;
    return frame;
}
@end

@implementation VmAccessibilityProvider {
    int _fd;
    NSCondition *_condition;
    NSLock *_writeLock;
    NSDictionary *_state;
    NSDictionary *_elements;
    NSMutableDictionary *_auxiliaryElements;
    NSMutableDictionary *_auxiliaryRecords;
    VmAccessibilityRemoteElement *_root;
    VmAccessibilityRemoteElement *_status;
    id _parent;
    id _window;
    NSMutableDictionary<NSString *, VmAccessibilityClientRequest *> *_requests;
}
- (instancetype)initWithFileDescriptor:(int)fd {
    if ((self = [super init])) {
        _fd = fd;
        _condition = [NSCondition new];
        _writeLock = [NSLock new];
        _requests = [NSMutableDictionary dictionary];
        _auxiliaryElements = NSMutableDictionary.dictionary;
        _auxiliaryRecords = NSMutableDictionary.dictionary;
        _root = [VmAccessibilityRemoteElement new];
        _root.owner = self;
        _root.container = YES;
        _root.nodeID = @"application";
        _status = [VmAccessibilityRemoteElement new];
        _status.owner = self;
        _status.status = YES;
        _status.nodeID = @"status";
        _state = @{ @"pending": @YES, @"label": @"Virtual machine", @"status": @"Guest accessibility is updating.",
            @"frame": [NSValue valueWithRect:NSZeroRect], @"elements": @{} };
    }
    return self;
}
- (BOOL)send:(NSDictionary *)message {
    [_writeLock lock];
    BOOL ok = AsbAXWriteMessage(_fd, message);
    [_writeLock unlock];
    return ok;
}
- (void)begin {
    [NSAccessibilityRemoteUIElement setRemoteUIApp:YES];
    if ([_root respondsToSelector:@selector(accessibilitySetPresenterProcessIdentifier:)])
        [_root accessibilitySetPresenterProcessIdentifier:getppid()];
    NSData *token = [NSAccessibilityRemoteUIElement remoteTokenForLocalUIElement:_root];
    AXError (*enable)(bool) = dlsym(RTLD_DEFAULT, "_AXUIElementUseSecondaryAXThread");
    if (!token.length || !enable || enable(true) != kAXErrorSuccess) exit(1);
    [self send:@{ @"type": @"ready", @"token": [token base64EncodedStringWithOptions:0] }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) {
            @autoreleasepool {
                NSDictionary *message = AsbAXReadMessage(self->_fd);
                if (!message) exit(0);
                dispatch_async(dispatch_get_main_queue(), ^{ [self receive:message]; });
            }
        }
    });
}
- (void)receive:(NSDictionary *)message {
    if ([message[@"type"] isEqual:@"actionResult"]) {
        [_condition lock];
        VmAccessibilityClientRequest *request = _requests[message[@"requestId"]];
        request.response = message;
        if (request) dispatch_semaphore_signal(request.signal);
        [_condition unlock];
        return;
    }
    if ([message[@"type"] isEqual:@"attach"]) {
        NSData *parent = [[NSData alloc] initWithBase64EncodedString:message[@"parent"] options:0];
        NSData *window = [[NSData alloc] initWithBase64EncodedString:message[@"window"] options:0];
        id remoteParent = [[NSAccessibilityRemoteUIElement alloc] initWithRemoteToken:parent];
        id remoteWindow = [[NSAccessibilityRemoteUIElement alloc] initWithRemoteToken:window];
        [remoteParent setWindowUIElement:remoteWindow];
        [remoteParent setTopLevelUIElement:remoteWindow];
        [_condition lock];
        _parent = remoteParent;
        _window = remoteWindow;
        [_condition unlock];
        return;
    }
    if (![message[@"type"] isEqual:@"tree"]) return;
    NSArray *raw = message[@"frame"];
    if (!AsbAXFiniteArray(raw, 4, NO)) return;
    NSRect frame = NSMakeRect([raw[0] doubleValue], [raw[1] doubleValue], [raw[2] doubleValue], [raw[3] doubleValue]);
    NSDictionary *previous = [self stateWaiting:NO];
    NSDictionary *previousElements = _elements;
    NSDictionary *previousAuxiliaryElements = nil;
    VmAccessibilitySnapshot *snapshot = message[@"snapshot"] ? VmAccessibilityParseSnapshot(message[@"snapshot"]) :
        ([message[@"mirroring"] boolValue] || [message[@"pending"] boolValue] ? previous[@"snapshot"] : nil);
    BOOL mirroring = [message[@"mirroring"] boolValue] && snapshot != nil;
    NSDictionary *elements = @{};
    if (!mirroring && [message[@"pending"] boolValue]) {
        snapshot = previous[@"snapshot"];
        elements = previous[@"elements"] ?: @{};
    }
    [_condition lock];
    if (mirroring && message[@"snapshot"]) {
        NSMutableDictionary *updatedElements = [NSMutableDictionary dictionary];
        [_auxiliaryRecords removeAllObjects];
        if (![snapshot.session isEqual:[previous[@"snapshot"] session]]) {
            previousAuxiliaryElements = [_auxiliaryElements copy];
            [_auxiliaryElements removeAllObjects];
        }
        for (NSString *nodeID in snapshot.nodes) {
            VmAccessibilityRemoteElement *element = _elements[nodeID] ?: _auxiliaryElements[nodeID];
            if (element.retired || ![element.session isEqual:snapshot.session]) element = nil;
            if (!element) {
                element = [VmAccessibilityRemoteElement new];
                element.owner = self;
                element.nodeID = nodeID;
                element.session = snapshot.session;
            }
            updatedElements[nodeID] = element;
        }
        elements = updatedElements;
        _elements = elements;
    } else if (mirroring) elements = previous[@"elements"] ?: @{};
    NSString *name = [snapshot.app[@"name"] length] ? snapshot.app[@"name"] : snapshot.app[@"bundleId"];
    NSMutableDictionary *state = [@{ @"mirroring": @(mirroring), @"pending": message[@"pending"] ?: @NO,
        @"navigationQueries": message[@"navigationQueries"] ?: @NO,
        @"epoch": message[@"epoch"] ?: @0, @"frame": [NSValue valueWithRect:frame],
        @"keyWindow": message[@"keyWindow"] ?: @NO, @"elements": elements,
        @"label": name ? [NSString stringWithFormat:@"%@ in %@", name, message[@"name"]] : message[@"name"],
        @"status": message[@"status"] ?: @"Guest accessibility is unavailable." } mutableCopy];
    if (snapshot) state[@"snapshot"] = snapshot;
    _state = state;
    [_condition broadcast];
    [_condition unlock];
    VmAccessibilitySnapshot *oldSnapshot = previous[@"snapshot"];
    BOOL newSession = snapshot && ![oldSnapshot.session isEqual:snapshot.session];
    BOOL changed = newSession || ![previous[@"mirroring"] isEqual:state[@"mirroring"]] || ![previous[@"frame"] isEqual:state[@"frame"]] ||
        ![oldSnapshot.roots isEqual:snapshot.roots] || oldSnapshot.nodes.count != snapshot.nodes.count;
    for (VmAccessibilityElement *element in previousAuxiliaryElements.allValues)
        if (elements[element.nodeID] != element) [element retire];
    if (mirroring && message[@"snapshot"]) {
        for (NSString *identifier in previousElements) {
            if (previousElements[identifier] != elements[identifier]) {
                [previousElements[identifier] retire];
                changed = YES;
            }
        }
    }
    if (mirroring && message[@"snapshot"]) {
        for (NSString *identifier in snapshot.nodes) {
            NSDictionary *old = newSession ? nil : oldSnapshot.nodes[identifier], *current = snapshot.nodes[identifier];
            if (![old[@"children"] isEqual:current[@"children"]] ||
                ![(old[@"attributes"][@"AXChildren"] ?: NSNull.null)
                    isEqual:(current[@"attributes"][@"AXChildren"] ?: NSNull.null)]) changed = YES;
            if (old && ![VmAccessibilityRecordValue(old) isEqual:VmAccessibilityRecordValue(current)])
                NSAccessibilityPostNotification(elements[identifier], NSAccessibilityValueChangedNotification);
            if (old && ![(old[@"selectedTextRange"] ?: NSNull.null) isEqual:(current[@"selectedTextRange"] ?: NSNull.null)])
                NSAccessibilityPostNotification(elements[identifier], NSAccessibilitySelectedTextChangedNotification);
        }
    }
    if (changed && ![state[@"pending"] boolValue])
        NSAccessibilityPostNotification(_root, NSAccessibilityLayoutChangedNotification);
    if (mirroring && [message[@"keyWindow"] boolValue] && snapshot.focused &&
        (newSession || ![oldSnapshot.focused isEqual:snapshot.focused]))
        NSAccessibilityPostNotification(elements[snapshot.focused], NSAccessibilityFocusedUIElementChangedNotification);
}
- (NSDictionary *)stateWaiting:(BOOL)wait {
    [_condition lock];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:0.2];
    while (wait && !NSThread.isMainThread && [_state[@"pending"] boolValue])
        if (![_condition waitUntilDate:deadline]) break;
    NSDictionary *state = _state;
    [_condition unlock];
    return state;
}
- (id)window { [_condition lock]; id window = _window; [_condition unlock]; return window; }
- (BOOL)isCurrentElement:(VmAccessibilityElement *)element {
    NSDictionary *state = [self stateWaiting:NO];
    if (element == _root) return YES;
    if (element == _status) return ![state[@"mirroring"] boolValue];
    if (state[@"elements"][element.nodeID] == element) return YES;
    [_condition lock];
    BOOL current = _auxiliaryElements[element.nodeID] == element && [[state[@"snapshot"] session] isEqual:element.session];
    [_condition unlock];
    return current;
}
- (NSDictionary *)recordForElement:(VmAccessibilityElement *)element {
    if (!element) return nil;
    NSDictionary *state = [self stateWaiting:NO];
    if (element == _root) return @{ @"label": state[@"label"], @"enabled": @YES,
        @"attributes": @{@"AXElementBusy": state[@"pending"] ?: @NO} };
    if (element == _status) return @{ @"label": state[@"status"],
        @"role": [state[@"pending"] boolValue] ? NSAccessibilityBusyIndicatorRole : NSAccessibilityStaticTextRole,
        @"enabled": @NO };
    VmAccessibilitySnapshot *snapshot = state[@"snapshot"];
    if (state[@"elements"][element.nodeID] == element) return snapshot.nodes[element.nodeID];
    [_condition lock];
    id cached = _auxiliaryRecords[element.nodeID];
    [_condition unlock];
    if (cached) return cached == NSNull.null ? nil : cached;
    if (NSThread.isMainThread || ![self isCurrentElement:element]) return nil;
    NSDictionary *response = [self request:@{@"action": @"getMetadata"} element:element];
    NSDictionary *record = [response[@"ok"] boolValue] ? VmAccessibilityParseRecord(response[@"record"]) : nil;
    if (![record[@"id"] isEqual:element.nodeID]) record = nil;
    [_condition lock];
    VmAccessibilitySnapshot *current = _state[@"snapshot"];
    if (current != snapshot || ![current.session isEqual:element.session]) record = nil;
    else _auxiliaryRecords[element.nodeID] = record ?: (id)NSNull.null;
    [_condition unlock];
    return record;
}
- (NSArray *)childrenForElement:(VmAccessibilityElement *)element {
    NSDictionary *state = [self stateWaiting:element == _root];
    if (element == _root && ![state[@"mirroring"] boolValue]) return @[_status];
    VmAccessibilitySnapshot *snapshot = state[@"snapshot"];
    NSArray *ids = element == _root ? snapshot.roots : [self recordForElement:element][@"children"];
    NSMutableArray *children = [NSMutableArray array];
    for (NSString *nodeID in ids) {
        id child = [self elementForIdentifier:nodeID session:snapshot.session];
        if (child) [children addObject:child];
    }
    return children;
}
- (id)parentForElement:(VmAccessibilityElement *)element {
    if (element == _root) { [_condition lock]; id parent = _parent; [_condition unlock]; return parent; }
    if (element == _status) return _root;
    NSDictionary *state = [self stateWaiting:NO];
    VmAccessibilitySnapshot *snapshot = state[@"snapshot"];
    NSString *parent = [self recordForElement:element][@"parent"];
    return parent ? [self elementForIdentifier:parent session:snapshot.session] : _root;
}
- (NSRect)frameForElement:(VmAccessibilityElement *)element {
    NSDictionary *state = [self stateWaiting:NO];
    if (![state[@"navigationQueries"] boolValue]) state = [self stateWaiting:!element.status];
    NSRect frame = [state[@"frame"] rectValue];
    if (element == _root || element == _status) return frame;
    VmAccessibilitySnapshot *snapshot = state[@"snapshot"];
    NSArray *raw;
    if ([state[@"pending"] boolValue]) {
        if (NSThread.isMainThread) return NSZeroRect;
        NSDictionary *response = [self request:@{@"action": @"getFrame"} element:element];
        if (![response[@"ok"] boolValue] || !AsbAXFiniteArray(response[@"value"], 4, NO)) return NSZeroRect;
        NSDictionary *current = [self stateWaiting:NO];
        if (![current[@"epoch"] isEqual:state[@"epoch"]] || ![self isCurrentElement:element]) return NSZeroRect;
        raw = response[@"value"];
        frame = [current[@"frame"] rectValue];
    } else raw = [self recordForElement:element][@"frame"];
    if (!raw || snapshot.pixels.width <= 0 || snapshot.pixels.height <= 0) return NSZeroRect;
    NSRect guest = NSMakeRect([raw[0] doubleValue], [raw[1] doubleValue], [raw[2] doubleValue], [raw[3] doubleValue]);
    if (NSIsEmptyRect(guest)) return NSZeroRect;
    return [VmAccessibilityTransformGeometry([NSValue valueWithRect:guest], snapshot, frame, NO) rectValue];
}
- (id)focusedGuestElement {
    NSDictionary *state = [self stateWaiting:NO];
    VmAccessibilitySnapshot *snapshot = state[@"snapshot"];
    return [state[@"keyWindow"] boolValue] && snapshot.focused ? state[@"elements"][snapshot.focused] : nil;
}
- (id)hitTest:(NSPoint)point {
    NSDictionary *state = [self stateWaiting:NO];
    VmAccessibilitySnapshot *snapshot = state[@"snapshot"];
    if (!snapshot || !NSPointInRect(point, [state[@"frame"] rectValue]) || NSThread.isMainThread) return nil;
    VmAccessibilityElement *target = state[@"elements"][snapshot.roots.firstObject];
    NSValue *guestPoint = [self transformGeometry:[NSValue valueWithPoint:point] toGuest:YES];
    id wire = guestPoint ? AsbAXEncodeValue(guestPoint, nil, 0) : nil;
    NSDictionary *response = wire ? [self request:@{@"action": @"hitTest", @"value": wire} element:target] : nil;
    id reference = response[@"value"];
    if (![response[@"ok"] boolValue] || ![reference isKindOfClass:NSDictionary.class] ||
        ![reference[@"$ax"] isEqual:@"element"] || !VmAccessibilityIsIdentifier(reference[@"value"])) return nil;
    NSDictionary *current = [self stateWaiting:NO];
    if (![current[@"epoch"] isEqual:state[@"epoch"]] || ![snapshot.session isEqual:[current[@"snapshot"] session]]) return nil;
    NSString *identifier = reference[@"value"];
    VmAccessibilityElement *element = [self elementForIdentifier:identifier session:snapshot.session];
    return [self recordForElement:element] ? element : nil;
}
- (id)elementForIdentifier:(NSString *)identifier session:(NSString *)session {
    if (!VmAccessibilityIsIdentifier(identifier)) return nil;
    [_condition lock];
    NSDictionary *state = _state;
    if (![[state[@"snapshot"] session] isEqual:session]) { [_condition unlock]; return nil; }
    id element = state[@"elements"][identifier];
    if (element) { [_condition unlock]; return element; }
    VmAccessibilityRemoteElement *auxiliary = _auxiliaryElements[identifier];
    if (auxiliary.retired) auxiliary = nil;
    if (!auxiliary && _auxiliaryElements.count < ASB_AX_MAX_NODES) {
        auxiliary = [VmAccessibilityRemoteElement new];
        auxiliary.owner = self;
        auxiliary.nodeID = identifier;
        auxiliary.session = session;
        _auxiliaryElements[identifier] = auxiliary;
    }
    [_condition unlock];
    return auxiliary;
}
- (NSValue *)transformGeometry:(NSValue *)value toGuest:(BOOL)toGuest {
    NSDictionary *state = [self stateWaiting:NO];
    return VmAccessibilityTransformGeometry(value, state[@"snapshot"], [state[@"frame"] rectValue], toGuest);
}
- (NSDictionary *)request:(NSDictionary *)operation element:(VmAccessibilityElement *)element {
    if (NSThread.isMainThread || ![self isCurrentElement:element] || element.container || element.status) return nil;
    NSTimeInterval deadline = VmAccessibilityNow() + ASB_AX_REQUEST_TIMEOUT_MS / 1000.0;
    NSString *requestID = NSUUID.UUID.UUIDString;
    VmAccessibilityClientRequest *request = [VmAccessibilityClientRequest new];
    request.signal = dispatch_semaphore_create(0);
    NSMutableDictionary *message = [operation mutableCopy];
    [message addEntriesFromDictionary:@{@"type": @"action", @"version": @(ASB_AX_VERSION), @"requestId": requestID,
        @"nodeId": element.nodeID, @"session": element.session, @"hostDeadline": @(deadline)}];
    [_condition lock];
    if (_requests.count >= ASB_AX_MAX_PENDING_REQUESTS) { [_condition unlock]; return nil; }
    _requests[requestID] = request;
    [_condition unlock];
    if ([self send:message]) {
        NSTimeInterval remaining = MAX(0, deadline - VmAccessibilityNow());
        dispatch_semaphore_wait(request.signal, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(remaining * NSEC_PER_SEC)));
    }
    [_condition lock];
    [_requests removeObjectForKey:requestID];
    NSDictionary *response = request.response;
    [_condition unlock];
    return response;
}
@end

int main(void) {
    @autoreleasepool {
        VmAccessibilityApplication *app = VmAccessibilityApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyProhibited];
        int one = 1;
        setsockopt(STDIN_FILENO, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        struct timeval timeout = { 1, 0 };
        setsockopt(STDIN_FILENO, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        VmAccessibilityProvider *server = [[VmAccessibilityProvider alloc] initWithFileDescriptor:STDIN_FILENO];
        app.accessibilityServer = server;
        dispatch_async(dispatch_get_main_queue(), ^{ [server begin]; });
        [app run];
    }
    return 0;
}
