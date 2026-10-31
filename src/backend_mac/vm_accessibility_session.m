#import "vm_accessibility_session.h"
#import "vm_accessibility_connection.h"
#import "vm_accessibility_presenter.h"
#import "vm_accessibility_snapshot.h"
#import "../../tools/transport/accessibility_value_mac.h"
#include <unistd.h>
#include <dlfcn.h>
#include <float.h>

@interface VmAccessibilitySession () <VmAccessibilityConnectionDelegate, VmAccessibilityPresenterDelegate>
- (void)publishSnapshot:(BOOL)includeSnapshot;
- (void)markSnapshotPending;
- (void)scheduleCapture;
- (void)attachWindow;
@end

static NSEvent *VmAccessibilityEventForWindow(CGEventRef input, NSWindow *window) {
    if (!input || !window) return nil;
    CGEventRef raw = CGEventCreateCopy(input);
    if (!raw) return nil;
    CGEventSetIntegerValueField(raw, kCGEventTargetUnixProcessID, getpid());
    NSEvent *event = [NSEvent eventWithCGEvent:raw];
    if (!event || !VmAccessibilityIsInputEvent(event)) { CFRelease(raw); return nil; }
    BOOL keyboard = event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp || event.type == NSEventTypeFlagsChanged;
    if (keyboard) {
        BOOL flags = event.type == NSEventTypeFlagsChanged;
        NSString *characters = flags ? @"" : event.characters ?: @"";
        if (!flags) {
            UniCharCount length = 0;
            CGEventKeyboardGetUnicodeString(raw, 0, &length, NULL);
            if (length && length <= 32768) {
                NSMutableData *buffer = [NSMutableData dataWithLength:length * sizeof(UniChar)];
                CGEventKeyboardGetUnicodeString(raw, length, &length, buffer.mutableBytes);
                characters = [[NSString alloc] initWithCharacters:buffer.bytes length:length];
            }
        }
        event = [NSEvent keyEventWithType:event.type location:NSZeroPoint modifierFlags:event.modifierFlags
            timestamp:event.timestamp windowNumber:window.windowNumber context:nil
            characters:characters
            charactersIgnoringModifiers:flags ? @"" : event.charactersIgnoringModifiers ?: @""
            isARepeat:flags ? NO : event.isARepeat keyCode:event.keyCode];
    } else if (event.windowNumber == window.windowNumber) {
        static void (*setWindowLocation)(CGEventRef, CGPoint);
        static dispatch_once_t once;
        dispatch_once(&once, ^{ setWindowLocation = dlsym(RTLD_DEFAULT, "CGEventSetWindowLocation"); });
        if (setWindowLocation) {
            CGPoint point = CGEventGetLocation(raw);
            NSPoint local = [window convertPointFromScreen:NSMakePoint(point.x, NSMaxY(NSScreen.screens.firstObject.frame) - point.y)];
            setWindowLocation(raw, CGPointMake(local.x, window.frame.size.height - local.y));
            event = [NSEvent eventWithCGEvent:raw];
        } else event = nil;
    } else event = nil;
    CFRelease(raw);
    return event;
}

@implementation VmAccessibilitySession {
    VmAccessibilityConnection *_connection;
    VmAccessibilityPresenter *_presenter;
    VmAccessibilitySnapshot *_snapshot;
    NSString *_name;
    NSString *_status;
    BOOL _started;
    BOOL _pending;
    BOOL _refreshScheduled;
    BOOL _displayAvailable;
    NSSize _displayPixels;
    uint64_t _requestedRefresh;
    uint64_t _epoch;
    NSUInteger _pendingActions;
    NSTimeInterval _lastInput;
    NSTimer *_watchdog;
    id _inputMonitor;
    NSMutableArray *_windowObservers;
}
- (instancetype)initWithView:(NSView *)view name:(NSString *)name {
    if ((self = [super init])) {
        _view = view;
        _name = [name copy] ?: @"virtual machine";
        _displayAvailable = YES;
        _status = @"Guest accessibility is unavailable.";
    }
    return self;
}
- (BOOL)connected { return _connection != nil; }
- (BOOL)hasAccessibilityContent { return _started && _presenter.root != nil; }
- (void)dealloc {
    [_connection stop];
    [_presenter stop];
    [_watchdog invalidate];
    if (_inputMonitor) [NSEvent removeMonitor:_inputMonitor];
    for (id token in _windowObservers) [NSNotificationCenter.defaultCenter removeObserver:token];
}
- (void)start {
    if (_started) return;
    _started = YES;
    _presenter = [VmAccessibilityPresenter new];
    _presenter.delegate = self;
    [_presenter start];
    [self observeWindow];
    __weak VmAccessibilitySession *weakSelf = self;
    _watchdog = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
        VmAccessibilitySession *session = weakSelf;
        if (!session) { [timer invalidate]; return; }
        if (session->_connection && VmAccessibilityNow() - session->_connection.lastActivity > 10) {
            VmAccessibilityConnection *connection = session->_connection;
            [connection stop];
            [session connectionEnded:connection];
        }
    }];
    [NSRunLoop.mainRunLoop addTimer:_watchdog forMode:NSRunLoopCommonModes];
}
- (void)stop {
    _started = NO;
    _epoch++;
    [_connection stop];
    _connection = nil;
    [_presenter stop];
    _presenter = nil;
    [_watchdog invalidate];
    _watchdog = nil;
    _snapshot = nil;
    _pending = NO;
    _pendingActions = 0;
    _refreshScheduled = NO;
    if (_inputMonitor) [NSEvent removeMonitor:_inputMonitor];
    _inputMonitor = nil;
    for (id token in _windowObservers) [NSNotificationCenter.defaultCenter removeObserver:token];
    [_windowObservers removeAllObjects];
}
- (BOOL)attachFileDescriptor:(int)fd {
    if (!_started) return NO;
    VmAccessibilityConnection *connection = [[VmAccessibilityConnection alloc] initWithFileDescriptor:fd];
    if (!connection) return NO;
    [_connection stop];
    _connection = connection;
    connection.delegate = self;
    _epoch++;
    _snapshot = nil;
    _requestedRefresh = 0;
    _pendingActions = 0;
    _refreshScheduled = NO;
    _pending = YES;
    _status = @"Guest accessibility is updating.";
    [self publishSnapshot:NO];
    [connection begin];
    return YES;
}
- (void)connectionEnded:(VmAccessibilityConnection *)connection {
    if (connection != _connection) return;
    _connection = nil;
    _epoch++;
    _snapshot = nil;
    _pending = NO;
    _status = @"Guest accessibility disconnected.";
    [self publishSnapshot:NO];
}
- (void)receiveState:(id)state connection:(VmAccessibilityConnection *)connection {
    if (!_started || connection != _connection) return;
    if ([state isKindOfClass:VmAccessibilitySnapshot.class]) {
        VmAccessibilitySnapshot *snapshot = state;
        if (connection.supportsRefresh && snapshot.refreshGeneration < _requestedRefresh) return;
        if (snapshot.truncated) {
            _snapshot = nil;
            _pending = NO;
            _status = @"The guest could not provide a complete accessibility tree.";
        } else {
            if ([_snapshot.session isEqual:snapshot.session] && snapshot.revision <= _snapshot.revision) return;
            _snapshot = snapshot;
            _pending = NO;
            _status = @"";
        }
        [self publishSnapshot:YES];
        return;
    }
    NSString *status = state[@"status"];
    if ([status isEqual:@"ready"]) return;
    if ([status isEqual:@"updating"]) {
        if ([state[@"invalidated"] boolValue] || !_snapshot) {
            _epoch++;
            _pending = YES;
            _status = @"Guest accessibility is updating.";
            [self publishSnapshot:NO];
        }
        return;
    }
    _snapshot = nil;
    _pending = NO;
    if ([status isEqual:@"permissionRequired"]) _status = @"Accessibility permission is required in the guest.";
    else if ([status isEqual:@"captureFailed"]) _status = @"The guest could not provide a complete accessibility tree.";
    else _status = @"Guest accessibility is unavailable.";
    [self publishSnapshot:NO];
}
- (void)noteInput {
    if (!_started || !_connection) return;
    _lastInput = VmAccessibilityNow();
    [self markSnapshotPending];
    [self scheduleCapture];
}
- (void)markSnapshotPending {
    _epoch++;
    _pending = YES;
    _status = @"Guest accessibility is updating.";
    if (_connection.supportsRefresh) _requestedRefresh++;
    [self publishSnapshot:NO];
}
- (void)scheduleCapture {
    if (_refreshScheduled || !_connection.supportsRefresh || _pendingActions) return;
    _refreshScheduled = YES;
    VmAccessibilityConnection *connection = _connection;
    __weak VmAccessibilitySession *weakSelf = self;
    double delay = MAX(0, ASB_AX_INPUT_SETTLE_MS / 1000.0 - (VmAccessibilityNow() - _lastInput));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        VmAccessibilitySession *session = weakSelf;
        if (!session || !session->_started || session->_connection != connection) return;
        session->_refreshScheduled = NO;
        if (session->_pendingActions) return;
        if (VmAccessibilityNow() - session->_lastInput < ASB_AX_INPUT_SETTLE_MS / 1000.0) [session scheduleCapture];
        else [connection refresh:session->_requestedRefresh];
    });
}
- (void)publishSnapshot:(BOOL)includeSnapshot {
    if (!_started || !_presenter.root || !self.view.window) return;
    NSRect frame = NSAccessibilityFrameInView(self.view, self.view.bounds);
    BOOL geometry = _displayAvailable && !self.view.window.miniaturized &&
        (!_snapshot || VmAccessibilityDisplayPixelsCompatible(_displayPixels, _snapshot.pixels));
    NSMutableDictionary *message = [@{@"type": @"tree", @"epoch": @(_epoch),
        @"mirroring": @(_snapshot && !_pending && geometry), @"pending": @(_pending && _connection != nil),
        @"navigationQueries": @(_connection.supportsNavigationQueries),
        @"frame": @[@(frame.origin.x), @(frame.origin.y), @(frame.size.width), @(frame.size.height)],
        @"keyWindow": @(self.view.window.keyWindow), @"name": _name, @"status": _status ?: @""} mutableCopy];
    if (includeSnapshot && _snapshot) message[@"snapshot"] = _snapshot.message;
    [_presenter send:message];
}
- (void)receiveRemoteMessage:(NSDictionary *)message presenter:(VmAccessibilityPresenter *)presenter {
    if (!_started || presenter != _presenter) return;
    NSString *type = message[@"type"];
    if ([type isEqual:@"ready"]) {
        [self attachWindow];
        [self publishSnapshot:YES];
        NSAccessibilityPostNotification(self.view, NSAccessibilityLayoutChangedNotification);
    } else if ([type isEqual:@"input"]) {
        if (!VmAccessibilityIsString(message[@"event"], 65536)) return;
        NSData *data = [[NSData alloc] initWithBase64EncodedString:message[@"event"] options:0];
        CGEventRef raw = data.length ? CGEventCreateFromData(NULL, (__bridge CFDataRef)data) : NULL;
        NSEvent *event = raw ? VmAccessibilityEventForWindow(raw, self.view.window) : nil;
        if (raw) CFRelease(raw);
        if (!event) return;
        [self.view.window makeFirstResponder:self.view];
        if (event.type != NSEventTypeMouseMoved) [self noteInput];
        [NSApp postEvent:event atStart:NO];
    } else if ([type isEqual:@"action"]) {
        NSString *requestID = message[@"requestId"], *action = message[@"action"], *attribute = message[@"attribute"];
        if (!VmAccessibilityIsIdentifier(requestID)) return;
        BOOL allowed = _connection && [_snapshot.session isEqual:message[@"session"]] &&
            VmAccessibilityIsIdentifier(message[@"nodeId"]) && VmAccessibilityIsString(action, 256) && action.length;
        if (message[@"hostDeadline"])
            allowed = allowed && VmAccessibilityIsNumber(message[@"hostDeadline"], 0, DBL_MAX);
        BOOL query = [@[@"getAttribute", @"getParameterizedAttribute", @"getMetadata", @"isAttributeSettable", @"getFrame", @"hitTest"] containsObject:action];
        if ([action isEqual:@"setAttribute"] || [action isEqual:@"getAttribute"] ||
            [action isEqual:@"getParameterizedAttribute"] || [action isEqual:@"isAttributeSettable"])
            allowed = allowed && VmAccessibilityIsString(attribute, 256) && attribute.length;
        if (message[@"value"]) allowed = allowed && AsbAXWireValueValid(message[@"value"], 0);
        if (!allowed) {
            [presenter send:@{@"type": @"actionResult", @"requestId": requestID, @"ok": @NO, @"error": @(kAXErrorInvalidUIElement)}];
            return;
        }
        if (message[@"hostDeadline"] && [message[@"hostDeadline"] doubleValue] <= VmAccessibilityNow()) {
            [presenter send:@{@"type": @"actionResult", @"requestId": requestID, @"ok": @NO, @"error": @(kAXErrorCannotComplete)}];
            return;
        }
        BOOL mutation = !query || AsbAXParameterizedAttributeMutates(attribute);
        if (mutation) { _pendingActions++; [self markSnapshotPending]; }
        VmAccessibilityConnection *connection = _connection;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSDictionary *response = [connection request:message];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->_connection == connection && mutation) {
                    if (self->_pendingActions) self->_pendingActions--;
                    [self scheduleCapture];
                    [self.view.window makeFirstResponder:self.view];
                }
                NSMutableDictionary *reply = [response mutableCopy] ?: [@{@"ok": @NO, @"error": @(kAXErrorCannotComplete)} mutableCopy];
                reply[@"type"] = @"actionResult";
                reply[@"requestId"] = requestID;
                [presenter send:reply];
            });
        });
    }
}
- (void)attachWindow {
    if (!_presenter.root || !self.view.window) return;
    NSData *parent = [NSAccessibilityRemoteUIElement remoteTokenForLocalUIElement:self.view];
    NSData *window = [NSAccessibilityRemoteUIElement remoteTokenForLocalUIElement:self.view.window];
    if (!parent || !window) return;
    _presenter.root.windowUIElement = self.view.window;
    _presenter.root.topLevelUIElement = self.view.window;
    [_presenter send:@{@"type": @"attach", @"parent": [parent base64EncodedStringWithOptions:0], @"window": [window base64EncodedStringWithOptions:0]}];
}
- (void)observeWindow {
    for (id token in _windowObservers) [NSNotificationCenter.defaultCenter removeObserver:token];
    _windowObservers = NSMutableArray.array;
    if (_inputMonitor) [NSEvent removeMonitor:_inputMonitor];
    _inputMonitor = nil;
    if (!_started || !self.view.window) return;
    __weak VmAccessibilitySession *weakSelf = self;
    _inputMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged |
        NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
        NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
        NSEventMaskOtherMouseDragged | NSEventMaskScrollWheel handler:^NSEvent *(NSEvent *event) {
        VmAccessibilitySession *session = weakSelf;
        BOOL keyboard = event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp || event.type == NSEventTypeFlagsChanged;
        if (session && (event.window == session.view.window || (keyboard && !event.window && NSApp.keyWindow == session.view.window))) [session noteInput];
        return event;
    }];
    for (NSNotificationName name in @[NSWindowDidMoveNotification, NSWindowDidResizeNotification, NSWindowDidChangeBackingPropertiesNotification,
        NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification, NSWindowDidMiniaturizeNotification, NSWindowDidDeminiaturizeNotification]) {
        [_windowObservers addObject:[NSNotificationCenter.defaultCenter addObserverForName:name object:self.view.window
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
                if ([notification.name isEqual:NSWindowDidBecomeKeyNotification] ||
                    [notification.name isEqual:NSWindowDidDeminiaturizeNotification]) [weakSelf attachWindow];
                [weakSelf geometryDidChange];
            }]];
    }
    [_windowObservers addObject:[NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification
        object:NSApp queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf attachWindow];
            [weakSelf geometryDidChange];
        }]];
    [self attachWindow];
    [self geometryDidChange];
}
- (void)geometryDidChange { [self publishSnapshot:NO]; }
- (void)setDisplayPixels:(NSSize)pixels available:(BOOL)available {
    BOOL changed = !NSEqualSizes(pixels, _displayPixels);
    _displayPixels = pixels;
    _displayAvailable = available;
    if (changed && available) [self noteInput];
    [self geometryDidChange];
}
- (NSString *)accessibilityLabel { return _name; }
- (NSArray *)accessibilityChildren { return _presenter.root ? @[_presenter.root] : @[]; }
- (id)accessibilityFocusedUIElement { return _presenter.root; }
- (id)accessibilityHitTest:(NSPoint)point {
    return NSPointInRect(point, NSAccessibilityFrameInView(self.view, self.view.bounds)) ? _presenter.root : nil;
}
@end
