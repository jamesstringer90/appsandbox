#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#import "../transport/accessibility_attributes_mac.h"
#import "../transport/accessibility_value_mac.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <math.h>

#if __has_include(<sys/vsock.h>)
#include <sys/vsock.h>
#else
#define AF_VSOCK 40
#define VMADDR_CID_HOST 2
struct sockaddr_vm {
    unsigned char svm_len;
    sa_family_t svm_family;
    unsigned short svm_reserved1;
    unsigned int svm_port;
    unsigned int svm_cid;
} __attribute__((__packed__));
#endif

static volatile sig_atomic_t g_stop;

static double GuestAccessibilityMonotonicTime(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static void GuestAccessibilityStopSignal(int value) {
    (void)value;
    g_stop = 1;
}

static NSString *GuestAccessibilityLimitedString(id value, NSUInteger maximum) {
    if (![value isKindOfClass:NSString.class]) return nil;
    NSString *s = value;
    if (s.length <= maximum) return s;
    NSUInteger length = maximum;
    if (CFStringIsSurrogateHighCharacter([s characterAtIndex:length - 1])) length--;
    return [s substringToIndex:length];
}

static NSString *GuestAccessibilityString(id value) { return GuestAccessibilityLimitedString(value, ASB_AX_MAX_STRING); }

static id GuestAccessibilityValueWithoutError(id value) {
    if (!value || value == NSNull.null) return nil;
    if (CFGetTypeID((__bridge CFTypeRef)value) == AXValueGetTypeID() &&
        AXValueGetType((__bridge AXValueRef)value) == kAXValueAXErrorType) return nil;
    return value;
}

static BOOL GuestAccessibilityIsUnexpectedError(id value) {
    if (!value || CFGetTypeID((__bridge CFTypeRef)value) != AXValueGetTypeID() ||
        AXValueGetType((__bridge AXValueRef)value) != kAXValueAXErrorType) return NO;
    AXError error = kAXErrorSuccess;
    return AXValueGetValue((__bridge AXValueRef)value, kAXValueAXErrorType, &error) &&
        error != kAXErrorSuccess && error != kAXErrorNoValue && error != kAXErrorAttributeUnsupported;
}

static BOOL GuestAccessibilityIsMissingAttribute(id value) {
    if (value == NSNull.null) return YES;
    if (!value || CFGetTypeID((__bridge CFTypeRef)value) != AXValueGetTypeID() ||
        AXValueGetType((__bridge AXValueRef)value) != kAXValueAXErrorType) return NO;
    AXError error = kAXErrorSuccess;
    return AXValueGetValue((__bridge AXValueRef)value, kAXValueAXErrorType, &error) &&
        (error == kAXErrorNoValue || error == kAXErrorAttributeUnsupported);
}

static NSArray *GuestAccessibilityReadAttributes(AXUIElementRef element, NSArray *attributes, AXError *outError) {
    if (!attributes.count) {
        if (outError) *outError = kAXErrorSuccess;
        return @[];
    }
    CFArrayRef values = NULL;
    AXError error = AXUIElementCopyMultipleAttributeValues(element,
        (__bridge CFArrayRef)attributes, 0, &values);
    if (outError) *outError = error;
    if (error != kAXErrorSuccess || !values) {
        if (values) CFRelease(values);
        return nil;
    }
    return CFBridgingRelease(values);
}

static NSArray *GuestAccessibilityReadSupportedAttributes(AXUIElementRef element, NSArray *attributes,
                                                         NSArray *supported, AXError *outError) {
    NSMutableArray *requested = NSMutableArray.array;
    for (NSString *name in attributes)
        if ([supported containsObject:name]) [requested addObject:name];
    NSArray *values = GuestAccessibilityReadAttributes(element, requested, outError);
    if (!values || values.count != requested.count) return nil;
    NSMutableArray *expanded = [NSMutableArray arrayWithCapacity:attributes.count];
    NSUInteger index = 0;
    for (NSString *name in attributes)
        [expanded addObject:[supported containsObject:name] ? values[index++] : NSNull.null];
    return expanded;
}

static NSArray *GuestAccessibilityElementArray(id value) {
    value = GuestAccessibilityValueWithoutError(value);
    if (!value) return @[];
    if (CFGetTypeID((__bridge CFTypeRef)value) == AXUIElementGetTypeID()) return @[value];
    return [value isKindOfClass:NSArray.class] ? value : @[];
}

static NSDictionary *GuestAccessibilityCaptureContext(void) {
    __block NSDictionary *context;
    dispatch_sync(dispatch_get_main_queue(), ^{
        struct stat info;
        BOOL active = stat("/dev/console", &info) == 0 && info.st_uid == getuid();
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session) active = NO;
        else {
            CFBooleanRef onConsole = CFDictionaryGetValue(session, kCGSessionOnConsoleKey);
            active = active && onConsole && CFGetTypeID(onConsole) == CFBooleanGetTypeID()
                && CFBooleanGetValue(onConsole);
            CFRelease(session);
        }
        NSRunningApplication *application = NSWorkspace.sharedWorkspace.frontmostApplication;
        NSScreen *screen = nil;
        CGDirectDisplayID displayID = CGMainDisplayID();
        for (NSScreen *candidate in NSScreen.screens) {
            if ([candidate.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == displayID) {
                screen = candidate;
                break;
            }
        }
        screen = screen ?: NSScreen.screens.firstObject;
        NSRect frame = screen ? screen.frame : NSZeroRect;
        if (screen) displayID = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
        size_t pixelWidth = mode ? CGDisplayModeGetPixelWidth(mode) : CGDisplayPixelsWide(displayID);
        size_t pixelHeight = mode ? CGDisplayModeGetPixelHeight(mode) : CGDisplayPixelsHigh(displayID);
        if (mode) CGDisplayModeRelease(mode);
        context = @{
            @"active": @(active && screen),
            @"trusted": @(AXIsProcessTrusted()),
            @"app": @{@"pid": @(application.processIdentifier),
                      @"bundleId": GuestAccessibilityLimitedString(application.bundleIdentifier, 1024) ?: @"",
                      @"name": GuestAccessibilityLimitedString(application.localizedName, 1024) ?: @""},
            @"display": @{@"x": @(frame.origin.x), @"y": @0,
                          @"width": @(frame.size.width), @"height": @(frame.size.height),
                          @"pixelWidth": @(pixelWidth),
                          @"pixelHeight": @(pixelHeight)}
        };
    });
    if ([context[@"active"] boolValue] && [context[@"trusted"] boolValue] &&
        [context[@"app"][@"pid"] intValue] <= 0) {
        AXUIElementRef system = AXUIElementCreateSystemWide();
        AXUIElementSetMessagingTimeout(system, 0.050f);
        CFTypeRef focused = NULL;
        pid_t pid = 0;
        AXError error = AXUIElementCopyAttributeValue(system, kAXFocusedApplicationAttribute, &focused);
        if (error == kAXErrorSuccess && focused && CFGetTypeID(focused) == AXUIElementGetTypeID())
            AXUIElementGetPid((AXUIElementRef)focused, &pid);
        if (focused) CFRelease(focused);
        CFRelease(system);
        if (pid > 0) {
            dispatch_sync(dispatch_get_main_queue(), ^{
                NSRunningApplication *application = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
                NSMutableDictionary *resolved = [context mutableCopy];
                resolved[@"app"] = @{@"pid": @(pid),
                    @"bundleId": GuestAccessibilityLimitedString(application.bundleIdentifier, 1024) ?: @"",
                    @"name": GuestAccessibilityLimitedString(application.localizedName, 1024) ?: @""};
                context = resolved;
            });
        }
    }
    return context;
}

@interface GuestAccessibilityRecord : NSObject
@property(nonatomic, readonly) AXUIElementRef element;
@property(nonatomic, copy) NSString *identifier;
@property(nonatomic, copy) NSArray<NSString *> *actions;
@property(nonatomic) uint64_t seenRevision;
@property(nonatomic) double attributeReadStartedAt;
@property(nonatomic) double lastSeenAt;
@property(nonatomic) pid_t applicationPID;
@property(nonatomic) BOOL secure;
@property(nonatomic) BOOL secureSticky;
@property(nonatomic, copy) NSArray *attributeNames;
@property(nonatomic, copy) NSArray *parameterizedNames;
@property(nonatomic, copy) NSArray *writableAttributes;
@property(nonatomic, copy) NSDictionary *settable;
@property(nonatomic, copy) NSDictionary *actionDescriptions;
@property(nonatomic) AXError capabilityError;
@property(nonatomic) AXError actionNamesError;
- (instancetype)initWithElement:(AXUIElementRef)element;
@end

@implementation GuestAccessibilityRecord
- (instancetype)initWithElement:(AXUIElementRef)element {
    if ((self = [super init])) {
        _element = (AXUIElementRef)CFRetain(element);
        AXUIElementSetMessagingTimeout(_element, 1.0f);
    }
    return self;
}
- (void)dealloc { if (_element) CFRelease(_element); }
@end

@interface GuestAccessibilityPendingAction : NSObject
@property(nonatomic, copy) NSDictionary *message;
@property(nonatomic) double preparedAt;
@end
@implementation GuestAccessibilityPendingAction
@end

@interface GuestAccessibilityCollector : NSObject
@property(nonatomic) int socket;
@property(nonatomic) BOOL subscribed;
@property(nonatomic) BOOL permissionRequested;
@property(nonatomic) double nextSnapshot;
@property(nonatomic) uint64_t revision;
@property(nonatomic) uint64_t publishedRevision;
@property(nonatomic) uint64_t refreshGeneration;
@property(nonatomic) uint64_t nextIdentifier;
@property(nonatomic) uint64_t workGeneration;
@property(nonatomic) uint64_t collectingGeneration;
@property(nonatomic) double captureDeadline;
@property(nonatomic) BOOL captureRequested;
@property(nonatomic) BOOL backgroundCaptureRequested;
@property(nonatomic) double backgroundCaptureAfter;
@property(nonatomic) NSUInteger captureFailures;
@property(nonatomic) BOOL connectionFailed;

@property(nonatomic) double nextHeartbeat;
@property(nonatomic, copy) NSString *session;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, NSMutableArray<GuestAccessibilityRecord *> *> *buckets;
@property(nonatomic, strong) NSMutableDictionary<NSString *, GuestAccessibilityRecord *> *records;
@property(nonatomic, copy) NSSet<NSString *> *publishedRecords;
@property(nonatomic, strong) NSMutableSet<NSString *> *capturingRecords;
@property(nonatomic, strong) NSMutableDictionary<NSString *, GuestAccessibilityPendingAction *> *pendingActions;
- (void)noteAccessibilityChange;
- (void)noteAccessibilityValueChange:(AXUIElementRef)element;
- (void)noteDestroyedAccessibilityElement:(AXUIElementRef)element;
@end

static void GuestAccessibilityObserveChange(AXObserverRef observer, AXUIElementRef element,
                             CFStringRef notification, void *context) {
    (void)observer;
    GuestAccessibilityCollector *exporter = (__bridge GuestAccessibilityCollector *)context;
    if (CFEqual(notification, kAXUIElementDestroyedNotification))
        [exporter noteDestroyedAccessibilityElement:element];
    else if (CFEqual(notification, kAXValueChangedNotification))
        [exporter noteAccessibilityValueChange:element];
    else [exporter noteAccessibilityChange];
}

@implementation GuestAccessibilityCollector {
    id _inputMonitor;
    id _applicationMonitor;
    AXObserverRef _accessibilityObserver;
    pid_t _observedPID;
    uint64_t _systemInput;
    uint64_t _observedSystemInput;
    double _systemInputAt;
    uint64_t _accessibilityChange;
    uint64_t _observedAccessibilityChange;
    double _accessibilityChangeAt;
    NSMutableArray<NSDictionary *> *_accessibilityValueChanges;
    BOOL _accessibilityValueChangesOverflow;
    NSMutableArray *_destroyedAccessibilityElements;
    BOOL _destroyedAccessibilityElementsOverflow;
}

- (instancetype)init {
    if ((self = [super init])) {
        _socket = -1;
        _buckets = NSMutableDictionary.dictionary;
        _records = NSMutableDictionary.dictionary;
        _pendingActions = NSMutableDictionary.dictionary;
    }
    return self;
}

- (void)dealloc {
    if (_inputMonitor) [NSEvent removeMonitor:_inputMonitor];
    if (_applicationMonitor) [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:_applicationMonitor];
    if (_accessibilityObserver) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(_accessibilityObserver), kCFRunLoopCommonModes);
        CFRelease(_accessibilityObserver);
    }
}

- (void)noteSystemInput {
    @synchronized(self) {
        _systemInput++;
        _systemInputAt = GuestAccessibilityMonotonicTime();
    }
}

- (void)observeSystemInput {
    if (!AXIsProcessTrusted()) return;
    __weak GuestAccessibilityCollector *weakSelf = self;
    if (!_inputMonitor) _inputMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp |
        NSEventMaskFlagsChanged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
        NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp |
        NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged | NSEventMaskScrollWheel
        handler:^(NSEvent *event) { (void)event; [weakSelf noteSystemInput]; }];
    if (!_applicationMonitor) _applicationMonitor = [NSWorkspace.sharedWorkspace.notificationCenter
        addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:nil
        usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf noteSystemInput]; }];
}

- (void)noteAccessibilityChange {
    @synchronized(self) {
        _accessibilityChange++;
        _accessibilityChangeAt = GuestAccessibilityMonotonicTime();
    }
}

- (void)noteAccessibilityValueChange:(AXUIElementRef)element {
    double observedAt = GuestAccessibilityMonotonicTime();
    @synchronized(self) {
        if (!_accessibilityValueChanges) _accessibilityValueChanges = NSMutableArray.array;
        if (_accessibilityValueChanges.count < 1024)
            [_accessibilityValueChanges addObject:@{@"element": (__bridge id)element, @"time": @(observedAt)}];
        else _accessibilityValueChangesOverflow = YES;
    }
}

- (void)processAccessibilityValueChanges {
    if (self.captureDeadline > 0) return;
    NSArray<NSDictionary *> *changes;
    BOOL overflow;
    @synchronized(self) {
        changes = _accessibilityValueChanges;
        _accessibilityValueChanges = nil;
        overflow = _accessibilityValueChangesOverflow;
        _accessibilityValueChangesOverflow = NO;
    }
    if (overflow) { [self noteAccessibilityChange]; return; }
    for (NSDictionary *change in changes) {
        AXUIElementRef element = (__bridge AXUIElementRef)change[@"element"];
        BOOL captured = NO;
        for (GuestAccessibilityRecord *record in self.buckets[@(CFHash(element))]) {
            if (!CFEqual(record.element, element)) continue;
            captured = self.publishedRevision && record.seenRevision == self.publishedRevision &&
                record.attributeReadStartedAt >= [change[@"time"] doubleValue];
            break;
        }
        if (!captured) { [self noteAccessibilityChange]; return; }
    }
}

- (void)noteDestroyedAccessibilityElement:(AXUIElementRef)element {
    @synchronized(self) {
        if (!_destroyedAccessibilityElements) _destroyedAccessibilityElements = NSMutableArray.array;
        if (_destroyedAccessibilityElements.count < 1024)
            [_destroyedAccessibilityElements addObject:(__bridge id)element];
        else _destroyedAccessibilityElementsOverflow = YES;
    }
}

- (void)processDestroyedAccessibilityElements {
    if (self.captureDeadline > 0) return;
    NSArray *destroyed;
    BOOL overflow;
    @synchronized(self) {
        destroyed = _destroyedAccessibilityElements;
        _destroyedAccessibilityElements = nil;
        overflow = _destroyedAccessibilityElementsOverflow;
        _destroyedAccessibilityElementsOverflow = NO;
    }
    if (overflow) { [self noteAccessibilityChange]; return; }
    for (id value in destroyed) {
        AXUIElementRef element = (__bridge AXUIElementRef)value;
        for (GuestAccessibilityRecord *record in self.buckets[@(CFHash(element))]) {
            if (record.seenRevision == self.revision && CFEqual(record.element, element)) {
                [self noteAccessibilityChange];
                return;
            }
        }
    }
}

- (void)observeApplication:(pid_t)pid {
    if (_accessibilityObserver && _observedPID == pid) return;
    if (_accessibilityObserver) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(_accessibilityObserver), kCFRunLoopCommonModes);
        CFRelease(_accessibilityObserver);
        _accessibilityObserver = NULL;
    }
    _observedPID = pid;
    if (pid <= 0 || AXObserverCreate(pid, GuestAccessibilityObserveChange, &_accessibilityObserver) != kAXErrorSuccess) return;
    AXUIElementRef application = AXUIElementCreateApplication(pid);
    AXUIElementSetMessagingTimeout(application, 0.050f);
    for (NSString *name in @[@"AXFocusedWindowChanged", @"AXFocusedUIElementChanged", @"AXWindowCreated",
        @"AXUIElementDestroyed", @"AXLayoutChanged", @"AXValueChanged", @"AXTitleChanged",
        @"AXSelectedChildrenChanged", @"AXSelectedRowsChanged", @"AXRowCountChanged",
        @"AXSelectedTextChanged", @"AXLoadComplete", @"AXLiveRegionChanged"])
        AXObserverAddNotification(_accessibilityObserver, application, (__bridge CFStringRef)name, (__bridge void *)self);
    CFRelease(application);
    CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(_accessibilityObserver), kCFRunLoopCommonModes);
}

- (void)processSystemInput {
    [self processAccessibilityValueChanges];
    [self processDestroyedAccessibilityElements];
    double inputAt, changeAt;
    BOOL inputChanged;
    @synchronized(self) {
        inputChanged = _observedSystemInput != _systemInput;
        if (!inputChanged && _observedAccessibilityChange == _accessibilityChange) return;
        _observedSystemInput = _systemInput;
        _observedAccessibilityChange = _accessibilityChange;
        inputAt = _systemInputAt;
        changeAt = _accessibilityChangeAt;
    }
    if (!self.subscribed) return;
    BOOL invalidate = inputChanged && !self.captureRequested && self.captureDeadline <= 0;
    if (inputChanged) {
        self.workGeneration++;
        self.captureFailures = 0;
        self.backgroundCaptureRequested = NO;
        self.nextSnapshot = inputAt + ASB_AX_INPUT_SETTLE_MS / 1000.0;
    } else {
        double next = MAX(self.backgroundCaptureAfter,
            MAX(inputAt + ASB_AX_INPUT_SETTLE_MS / 1000.0, changeAt + 0.1));
        if (!self.captureRequested) {
            if (self.captureDeadline <= 0) self.workGeneration++;
            self.backgroundCaptureRequested = YES;
            self.nextSnapshot = next;
        } else if (self.backgroundCaptureRequested) self.nextSnapshot = MIN(self.nextSnapshot, next);
    }
    self.captureRequested = YES;
    if (invalidate && ![self send:@{@"type": @"status", @"version": @(ASB_AX_VERSION), @"session": self.session,
        @"status": @"updating", @"refreshSupported": @YES, @"navigationQueriesSupported": @YES,
        @"actionConfirmationSupported": @YES, @"invalidated": @YES}])
        self.connectionFailed = YES;
}

- (NSDictionary *)context {
    NSDictionary *context = GuestAccessibilityCaptureContext();
    if ([context[@"trusted"] boolValue]) {
        pid_t pid = [context[@"app"][@"pid"] intValue];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self observeSystemInput];
            [self observeApplication:pid];
        });
    }
    return context;
}

- (BOOL)send:(NSDictionary *)message {
    return self.socket >= 0 && AsbAXWriteMessage(self.socket, message);
}

- (BOOL)sendStatus:(NSString *)status {
    return [self send:@{@"type": @"status", @"version": @(ASB_AX_VERSION),
                       @"session": self.session, @"status": status, @"refreshSupported": @YES,
                       @"navigationQueriesSupported": @YES, @"actionConfirmationSupported": @YES}];
}

- (int)connect {
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); return -1; }
    struct sockaddr_vm address = {0};
    address.svm_len = sizeof(address);
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_HOST;
    address.svm_port = ASB_AX_PORT;
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (result < 0) {
        double deadline = GuestAccessibilityMonotonicTime() + 2.0;
        BOOL connected = NO;
        while (!g_stop && GuestAccessibilityMonotonicTime() < deadline) {
            fd_set writes;
            FD_ZERO(&writes);
            FD_SET(fd, &writes);
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
            result = select(fd + 1, NULL, &writes, NULL, &timeout);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0) break;
            if (result == 0) continue;
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) connected = YES;
            break;
        }
        if (!connected) { close(fd); return -1; }
    }
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) { close(fd); return -1; }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

- (GuestAccessibilityRecord *)record:(AXUIElementRef)element applicationPID:(pid_t)pid {
    NSNumber *hash = @(CFHash(element));
    NSMutableArray *bucket = self.buckets[hash];
    for (GuestAccessibilityRecord *record in bucket) {
        if (CFEqual(record.element, element)) {
            record.lastSeenAt = GuestAccessibilityMonotonicTime();
            [self.capturingRecords addObject:record.identifier];
            return record;
        }
    }
    GuestAccessibilityRecord *record = [[GuestAccessibilityRecord alloc] initWithElement:element];
    record.identifier = [NSString stringWithFormat:@"%llu", (unsigned long long)++self.nextIdentifier];
    record.applicationPID = pid;
    record.lastSeenAt = GuestAccessibilityMonotonicTime();
    if (!bucket) self.buckets[hash] = bucket = NSMutableArray.array;
    [bucket addObject:record];
    self.records[record.identifier] = record;
    [self.capturingRecords addObject:record.identifier];
    return record;
}

- (void)prune:(NSSet<NSString *> *)retained {
    for (NSString *identifier in self.records.allKeys) {
        if ([retained containsObject:identifier]) continue;
        GuestAccessibilityRecord *record = self.records[identifier];
        NSNumber *hash = @(CFHash(record.element));
        NSMutableArray *bucket = self.buckets[hash];
        [bucket removeObjectIdenticalTo:record];
        if (!bucket.count) [self.buckets removeObjectForKey:hash];
        [self.records removeObjectForKey:identifier];
    }
}

- (void)expireRecords {
    double cutoff = GuestAccessibilityMonotonicTime() - 2.0;
    NSMutableSet *retained = NSMutableSet.set;
    for (GuestAccessibilityRecord *record in self.records.allValues)
        if (record.lastSeenAt >= cutoff || [self.publishedRecords containsObject:record.identifier] ||
            [self.capturingRecords containsObject:record.identifier]) [retained addObject:record.identifier];
    [self prune:retained];
}

- (void)updateCapabilities:(GuestAccessibilityRecord *)record deadline:(double)deadline {
    if (GuestAccessibilityMonotonicTime() >= deadline) { record.capabilityError = kAXErrorCannotComplete; return; }
    CFArrayRef actions = NULL, attributes = NULL, parameters = NULL;
    AXError actionError = AXUIElementCopyActionNames(record.element, &actions);
    AXError attributeError = AXUIElementCopyAttributeNames(record.element, &attributes);
    AXError parameterError = AXUIElementCopyParameterizedAttributeNames(record.element, &parameters);
    record.actionNamesError = actionError;
    record.capabilityError = kAXErrorSuccess;
    for (NSNumber *value in @[@(actionError == kAXErrorInvalidUIElement ? actionError : kAXErrorSuccess),
                              @(attributeError), @(parameterError)]) {
        AXError error = value.intValue;
        if (error == kAXErrorInvalidUIElement) { record.capabilityError = error; break; }
        if (error != kAXErrorSuccess && error != kAXErrorNotImplemented && error != kAXErrorAttributeUnsupported)
            record.capabilityError = error;
    }
    if (record.capabilityError != kAXErrorSuccess) {
        if (actions) CFRelease(actions);
        if (attributes) CFRelease(attributes);
        if (parameters) CFRelease(parameters);
        return;
    }
    record.actions = actionError == kAXErrorSuccess && actions ? CFBridgingRelease(actions) : @[];
    if (actionError != kAXErrorSuccess && actions) CFRelease(actions);
    if (attributeError == kAXErrorSuccess) record.attributeNames = CFBridgingRelease(attributes) ?: @[];
    else if (attributes) CFRelease(attributes);
    if (parameterError == kAXErrorSuccess || parameterError == kAXErrorNotImplemented || parameterError == kAXErrorAttributeUnsupported)
        record.parameterizedNames = CFBridgingRelease(parameters) ?: @[];
    else if (parameters) CFRelease(parameters);
    if (record.attributeNames) {
        NSMutableArray *writable = NSMutableArray.array;
        NSMutableDictionary *known = NSMutableDictionary.dictionary;
        for (NSString *attribute in record.attributeNames) {
            if (GuestAccessibilityMonotonicTime() >= deadline) { record.capabilityError = kAXErrorCannotComplete; return; }
            Boolean settable = false;
            AXError error = AXUIElementIsAttributeSettable(record.element, (__bridge CFStringRef)attribute, &settable);
            if (error == kAXErrorCannotComplete || error == kAXErrorInvalidUIElement) {
                record.capabilityError = error;
                return;
            }
            if (error == kAXErrorSuccess || error == kAXErrorAttributeUnsupported || error == kAXErrorNotImplemented) {
                known[attribute] = @(error == kAXErrorSuccess && settable);
                if ([known[attribute] boolValue]) [writable addObject:attribute];
            }
        }
        record.settable = known;
        record.writableAttributes = writable;
    }
    NSMutableDictionary *descriptions = NSMutableDictionary.dictionary;
    for (NSString *action in record.actions) {
        CFStringRef description = NULL;
        if (AXUIElementCopyActionDescription(record.element, (__bridge CFStringRef)action, &description) == kAXErrorSuccess && description)
            descriptions[action] = CFBridgingRelease(description);
        else if (description) CFRelease(description);
    }
    record.actionDescriptions = descriptions;
}

- (id)encodeValue:(id)value applicationPID:(pid_t)pid {
    return AsbAXEncodeValue(value, ^id(id candidate) {
        if (CFGetTypeID((__bridge CFTypeRef)candidate) != AXUIElementGetTypeID()) return nil;
        pid_t targetPID = 0;
        if (AXUIElementGetPid((__bridge AXUIElementRef)candidate, &targetPID) != kAXErrorSuccess || targetPID != pid) return NSNull.null;
        GuestAccessibilityRecord *record = [self record:(__bridge AXUIElementRef)candidate applicationPID:pid];
        return @{@"$ax": @"element", @"value": record.identifier};
    }, 0);
}

- (id)decodeValue:(id)value applicationPID:(pid_t)pid {
    return AsbAXDecodeValue(value, YES, ^id(NSString *identifier) {
        GuestAccessibilityRecord *record = self.records[identifier];
        return record.applicationPID == pid ? (__bridge id)record.element : nil;
    }, nil, 0);
}

- (NSDictionary *)metadataForRecord:(GuestAccessibilityRecord *)record {
    [self updateCapabilities:record deadline:GuestAccessibilityMonotonicTime() + ASB_AX_REQUEST_TIMEOUT_MS / 1000.0];
    if (record.capabilityError != kAXErrorSuccess || !record.attributeNames) return nil;
    NSArray *capturedNames = AsbAXCaptureAttributeNames(record.attributeNames);
    NSArray *values = GuestAccessibilityReadAttributes(record.element, capturedNames, NULL);
    if (values.count != capturedNames.count) return nil;
    NSUInteger subroleIndex = [capturedNames indexOfObject:@"AXSubrole"];
    if (subroleIndex != NSNotFound) {
        id subrole = GuestAccessibilityValueWithoutError(values[subroleIndex]);
        if ([subrole isKindOfClass:NSString.class]) {
            record.secure = [subrole isEqual:@"AXSecureTextField"];
            record.secureSticky = record.secure;
        } else record.secure = record.secureSticky || !GuestAccessibilityIsMissingAttribute(values[subroleIndex]);
    } else record.secure = record.secureSticky;
    NSMutableDictionary *attributes = NSMutableDictionary.dictionary;
    NSMutableDictionary *errors = NSMutableDictionary.dictionary;
    for (NSUInteger i = 0; i < values.count; i++) {
        NSString *name = capturedNames[i];
        if (GuestAccessibilityIsUnexpectedError(values[i])) {
            AXError error = kAXErrorFailure;
            AXValueGetValue((__bridge AXValueRef)values[i], kAXValueAXErrorType, &error);
            if (error == kAXErrorInvalidUIElement) return nil;
            errors[name] = @(error);
        }
        id value = GuestAccessibilityValueWithoutError(values[i]);
        if (!value) continue;
        if (record.secure && ([name containsString:@"Text"] || [name containsString:@"Character"] ||
            ([name isEqual:@"AXValue"] && !AsbAXIsMaskedValue(value)))) continue;
        id encoded = [self encodeValue:value applicationPID:record.applicationPID];
        if (encoded) attributes[name] = encoded;
    }
    NSMutableDictionary *node = [@{@"id": record.identifier, @"actions": record.actions ?: @[],
        @"attributeNames": record.attributeNames, @"parameterizedNames": record.secure ? @[] : (record.parameterizedNames ?: @[]),
        @"writableAttributes": record.writableAttributes ?: @[], @"settable": record.settable ?: @{}, @"actionDescriptions": record.actionDescriptions ?: @{},
        @"attributes": attributes, @"enabled": attributes[@"AXEnabled"] ?: @YES,
        @"focused": attributes[@"AXFocused"] ?: @NO, @"secure": @(record.secure),
        @"role": attributes[@"AXRole"] ?: @"AXUnknown", @"frame": @[@0, @0, @0, @0]} mutableCopy];
    if (errors.count) node[@"attributeErrors"] = errors;
    if (record.actionNamesError != kAXErrorSuccess) node[@"actionNamesError"] = @(record.actionNamesError);
    NSDictionary *keys = @{@"AXTitle": @"title", @"AXDescription": @"label", @"AXSubrole": @"subrole", @"AXValue": @"value"};
    for (NSString *key in keys) if (attributes[key]) node[keys[key]] = attributes[key];
    NSArray *position = attributes[@"AXPosition"][@"value"], *size = attributes[@"AXSize"][@"value"];
    if (AsbAXFiniteArray(position, 2, NO) && AsbAXFiniteArray(size, 2, NO))
        node[@"frame"] = @[position[0], position[1], size[0], size[1]];
    NSMutableArray *children = NSMutableArray.array;
    for (id child in attributes[@"AXChildren"]) if ([child isKindOfClass:NSDictionary.class] && [child[@"$ax"] isEqual:@"element"])
        [children addObject:child[@"value"]];
    node[@"children"] = children;
    if ([attributes[@"AXParent"][@"$ax"] isEqual:@"element"]) node[@"parent"] = attributes[@"AXParent"][@"value"];
    return node;
}

- (BOOL)collectionInterrupted {
    [self processSystemInput];
    if (![self expirePendingActions]) self.connectionFailed = YES;
    if (g_stop || self.connectionFailed || !self.subscribed || self.workGeneration != self.collectingGeneration) return YES;
    if (self.captureDeadline > 0 && GuestAccessibilityMonotonicTime() >= self.captureDeadline) return YES;
    if (GuestAccessibilityMonotonicTime() >= self.nextHeartbeat) {
        if (![self sendStatus:@"updating"]) { self.connectionFailed = YES; return YES; }
        self.nextHeartbeat = GuestAccessibilityMonotonicTime() + 0.5;
    }
    for (NSUInteger count = 0; count < ASB_AX_MAX_PENDING_REQUESTS; count++) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(self.socket, &reads);
        struct timeval timeout = {0, 0};
        int ready = select(self.socket + 1, &reads, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { self.connectionFailed = YES; return YES; }
        if (!ready) break;
        NSDictionary *message = AsbAXReadMessage(self.socket);
        if (!message || ![self handleMessage:message]) { self.connectionFailed = YES; return YES; }
        if (!self.subscribed || self.workGeneration != self.collectingGeneration ||
            (self.captureDeadline > 0 && GuestAccessibilityMonotonicTime() >= self.captureDeadline)) return YES;
    }
    return NO;
}

- (NSDictionary *)snapshot:(NSDictionary *)context {
    [self expireRecords];
    self.nextHeartbeat = GuestAccessibilityMonotonicTime() + 0.5;
    pid_t pid = [context[@"app"][@"pid"] intValue];
    AXUIElementRef application = AXUIElementCreateApplication(pid);
    AXUIElementSetMessagingTimeout(application, 1.0f);
    NSArray *rootValues = GuestAccessibilityReadAttributes(application, @[(__bridge NSString *)kAXMenuBarAttribute,
        (__bridge NSString *)kAXFocusedWindowAttribute, (__bridge NSString *)kAXWindowsAttribute], NULL);
    CFRelease(application);
    if (rootValues.count != 3) return nil;
    for (id value in rootValues) if (GuestAccessibilityIsUnexpectedError(value)) return nil;
    self.revision++;
    BOOL truncated = NO;
    NSMutableArray<GuestAccessibilityRecord *> *queue = NSMutableArray.array;
    NSMutableArray<NSString *> *roots = NSMutableArray.array;
    NSMutableSet<NSString *> *queued = NSMutableSet.set;
    NSMutableDictionary<NSString *, NSString *> *parents = NSMutableDictionary.dictionary;
    NSMutableDictionary<NSString *, NSNumber *> *depths = NSMutableDictionary.dictionary;
    for (id value in rootValues) {
        for (id element in GuestAccessibilityElementArray(value)) {
            if (CFGetTypeID((__bridge CFTypeRef)element) != AXUIElementGetTypeID()) continue;
            if (queue.count >= ASB_AX_MAX_NODES) { truncated = YES; break; }
            GuestAccessibilityRecord *record = [self record:(__bridge AXUIElementRef)element applicationPID:pid];
            if ([queued containsObject:record.identifier]) continue;
            [queued addObject:record.identifier];
            [queue addObject:record];
            [roots addObject:record.identifier];
            depths[record.identifier] = @0;
        }
    }
    NSArray *attributes = @[(__bridge NSString *)kAXRoleAttribute, (__bridge NSString *)kAXSubroleAttribute,
        (__bridge NSString *)kAXTitleAttribute, (__bridge NSString *)kAXDescriptionAttribute,
        (__bridge NSString *)kAXValueAttribute, (__bridge NSString *)kAXEnabledAttribute,
        (__bridge NSString *)kAXFocusedAttribute, (__bridge NSString *)kAXPositionAttribute,
        (__bridge NSString *)kAXSizeAttribute, (__bridge NSString *)kAXChildrenAttribute,
        (__bridge NSString *)kAXSelectedTextRangeAttribute];
    NSMutableArray<NSMutableDictionary *> *nodes = NSMutableArray.array;
    NSString *focused = nil;
    NSUInteger estimatedBytes = 2048;
    for (NSUInteger cursor = 0; cursor < queue.count; cursor++) {
        if (g_stop) return nil;
        if ([self collectionInterrupted]) return nil;
        if (estimatedBytes >= ASB_AX_MAX_MESSAGE * 3 / 4) {
            truncated = YES;
            break;
        }
        @autoreleasepool {
            GuestAccessibilityRecord *record = queue[cursor];
            AXUIElementSetMessagingTimeout(record.element, 1.0f);
            [self updateCapabilities:record deadline:self.captureDeadline > 0 ? self.captureDeadline : GuestAccessibilityMonotonicTime() + ASB_AX_CAPTURE_TIMEOUT_MS / 1000.0];
            if (record.capabilityError == kAXErrorInvalidUIElement) continue;
            if (record.capabilityError != kAXErrorSuccess) return nil;
            NSMutableArray *remaining = [AsbAXCaptureAttributeNames(record.attributeNames) mutableCopy];
            [remaining removeObjectsInArray:attributes];
            double attributeReadStartedAt = GuestAccessibilityMonotonicTime();
            AXError extraError = kAXErrorSuccess;
            NSArray *extraValues = remaining.count ? GuestAccessibilityReadAttributes(record.element, remaining, &extraError) : @[];
            if (extraError == kAXErrorInvalidUIElement) continue;
            if (extraError == kAXErrorSuccess && extraValues.count != remaining.count) return nil;
            AXError bulkError = kAXErrorSuccess;
            NSArray *values = GuestAccessibilityReadSupportedAttributes(record.element, attributes, record.attributeNames, &bulkError);
            if (bulkError == kAXErrorInvalidUIElement && ![roots containsObject:record.identifier]) continue;
            if (values.count != attributes.count) {
                return nil;
            }
            BOOL invalid = NO;
            NSMutableDictionary *attributeErrors = NSMutableDictionary.dictionary;
            for (NSUInteger index = 0; index < values.count; index++) {
                id value = values[index];
                if (!GuestAccessibilityIsUnexpectedError(value)) continue;
                AXError error = kAXErrorSuccess;
                AXValueGetValue((__bridge AXValueRef)value, kAXValueAXErrorType, &error);
                if (error == kAXErrorInvalidUIElement) {
                    if ([roots containsObject:record.identifier]) return nil;
                    invalid = YES;
                } else attributeErrors[attributes[index]] = @(error);
            }
            for (NSUInteger index = 0; index < extraValues.count; index++) {
                if (!GuestAccessibilityIsUnexpectedError(extraValues[index])) continue;
                AXError error = kAXErrorSuccess;
                AXValueGetValue((__bridge AXValueRef)extraValues[index], kAXValueAXErrorType, &error);
                if (error == kAXErrorInvalidUIElement) invalid = YES;
                else attributeErrors[remaining[index]] = @(error);
            }
            if (invalid) continue;
            if (extraError != kAXErrorSuccess)
                for (NSString *name in remaining) attributeErrors[name] = @(extraError);
            NSString *rawRole = GuestAccessibilityLimitedString(GuestAccessibilityValueWithoutError(values[0]), 128);
            NSString *role = [rawRole hasPrefix:@"AX"] ? rawRole : @"AXUnknown";
            NSString *subrole = GuestAccessibilityString(GuestAccessibilityValueWithoutError(values[1]));
            if (subrole.length) {
                record.secure = [subrole isEqualToString:(__bridge NSString *)kAXSecureTextFieldSubrole];
                record.secureSticky = record.secure;
            } else {
                record.secure = record.secureSticky || !GuestAccessibilityIsMissingAttribute(values[1]) ||
                    ![rawRole hasPrefix:@"AX"];
            }
            record.seenRevision = self.revision;
            record.attributeReadStartedAt = attributeErrors.count ? 0 : attributeReadStartedAt;
            NSMutableDictionary *node = [@{@"id": record.identifier, @"role": role,
                @"enabled": @YES, @"focused": @NO, @"secure": @(record.secure),
                @"frame": @[@0, @0, @0, @0]} mutableCopy];
            if (attributeErrors.count) node[@"attributeErrors"] = attributeErrors;
            if (parents[record.identifier]) node[@"parent"] = parents[record.identifier];
            if (subrole) node[@"subrole"] = subrole;
            NSString *title = [record.attributeNames containsObject:@"AXTitle"] ? GuestAccessibilityString(GuestAccessibilityValueWithoutError(values[2])) : nil;
            NSString *label = [record.attributeNames containsObject:@"AXDescription"] ? GuestAccessibilityString(GuestAccessibilityValueWithoutError(values[3])) : nil;
            id value = GuestAccessibilityValueWithoutError(values[4]);
            if (!record.secure) {
                if ([value isKindOfClass:NSString.class]) node[@"value"] = GuestAccessibilityString(value);
                else if ([value isKindOfClass:NSNumber.class] && isfinite([value doubleValue])) node[@"value"] = value;
            } else if (AsbAXIsMaskedValue(value)) node[@"value"] = value;
            id enabled = GuestAccessibilityValueWithoutError(values[5]);
            id isFocused = GuestAccessibilityValueWithoutError(values[6]);
            if ([enabled isKindOfClass:NSNumber.class]) node[@"enabled"] = @([enabled boolValue]);
            if ([isFocused isKindOfClass:NSNumber.class]) node[@"focused"] = @([isFocused boolValue]);
            if ([node[@"focused"] boolValue]) focused = record.identifier;
            id position = GuestAccessibilityValueWithoutError(values[7]);
            id size = GuestAccessibilityValueWithoutError(values[8]);
            CGPoint point = CGPointZero;
            CGSize extent = CGSizeZero;
            if (position && size && CFGetTypeID((__bridge CFTypeRef)position) == AXValueGetTypeID() &&
                CFGetTypeID((__bridge CFTypeRef)size) == AXValueGetTypeID() &&
                AXValueGetType((__bridge AXValueRef)position) == kAXValueCGPointType &&
                AXValueGetType((__bridge AXValueRef)size) == kAXValueCGSizeType &&
                AXValueGetValue((__bridge AXValueRef)position, kAXValueCGPointType, &point) &&
                AXValueGetValue((__bridge AXValueRef)size, kAXValueCGSizeType, &extent) &&
                isfinite(point.x) && isfinite(point.y) && isfinite(extent.width) && isfinite(extent.height))
                node[@"frame"] = @[@(point.x), @(point.y), @(MAX(0, extent.width)), @(MAX(0, extent.height))];
            id selectedRange = [record.attributeNames containsObject:@"AXSelectedTextRange"] ? GuestAccessibilityValueWithoutError(values[10]) : nil;
            CFRange range;
            if (!record.secure && selectedRange && CFGetTypeID((__bridge CFTypeRef)selectedRange) == AXValueGetTypeID() &&
                AXValueGetType((__bridge AXValueRef)selectedRange) == kAXValueCFRangeType &&
                AXValueGetValue((__bridge AXValueRef)selectedRange, kAXValueCFRangeType, &range) &&
                range.location >= 0 && range.length >= 0)
                node[@"selectedTextRange"] = @[@(range.location), @(range.length)];
            NSMutableArray *children = NSMutableArray.array;
            NSMutableArray<GuestAccessibilityRecord *> *childRecords = NSMutableArray.array;
            for (id child in GuestAccessibilityElementArray(values[9])) {
                if (CFGetTypeID((__bridge CFTypeRef)child) != AXUIElementGetTypeID()) continue;
                if (queued.count >= ASB_AX_MAX_NODES ||
                    [depths[record.identifier] unsignedIntegerValue] >= 127) { truncated = YES; break; }
                GuestAccessibilityRecord *next = [self record:(__bridge AXUIElementRef)child applicationPID:pid];
                if ([queued containsObject:next.identifier]) continue;
                [queued addObject:next.identifier];
                [childRecords addObject:next];
                parents[next.identifier] = record.identifier;
                depths[next.identifier] = @([depths[record.identifier] unsignedIntegerValue] + 1);
                [children addObject:next.identifier];
            }
            NSMutableSet<NSString *> *visibleIdentifiers = NSMutableSet.set;
            for (NSString *name in @[@"AXVisibleChildren", @"AXVisibleRows"]) {
                NSUInteger index = [remaining indexOfObject:name];
                id visible = index != NSNotFound && index < extraValues.count ? extraValues[index] : nil;
                for (id child in GuestAccessibilityElementArray(visible)) {
                    if (CFGetTypeID((__bridge CFTypeRef)child) != AXUIElementGetTypeID()) continue;
                    [visibleIdentifiers addObject:[self record:(__bridge AXUIElementRef)child applicationPID:pid].identifier];
                }
            }
            if (visibleIdentifiers.count) {
                NSMutableArray *ordered = NSMutableArray.array;
                for (GuestAccessibilityRecord *child in childRecords)
                    if ([visibleIdentifiers containsObject:child.identifier]) [ordered addObject:child];
                for (GuestAccessibilityRecord *child in childRecords)
                    if (![visibleIdentifiers containsObject:child.identifier]) [ordered addObject:child];
                childRecords = ordered;
            }
            if (childRecords.count)
                [queue insertObjects:childRecords atIndexes:[NSIndexSet indexSetWithIndexesInRange:
                    NSMakeRange(cursor + 1, childRecords.count)]];
            node[@"children"] = children;
            NSMutableDictionary *nativeValues = NSMutableDictionary.dictionary;
            id nativeChildren = GuestAccessibilityValueWithoutError(values[9]);
            id encodedChildren = nativeChildren ? AsbAXEncodeValue(nativeChildren, ^id(id candidate) {
                if (CFGetTypeID((__bridge CFTypeRef)candidate) != AXUIElementGetTypeID()) return nil;
                GuestAccessibilityRecord *child = [self record:(__bridge AXUIElementRef)candidate applicationPID:pid];
                return @{@"$ax": @"element", @"value": child.identifier};
            }, 0) : nil;
            if (encodedChildren) nativeValues[@"AXChildren"] = encodedChildren;
            if (value && (!record.secure || AsbAXIsMaskedValue(value))) {
                id encoded = [self encodeValue:value applicationPID:pid];
                if (encoded) nativeValues[@"AXValue"] = encoded;
            }
            for (NSUInteger index = 0; index < extraValues.count; index++) {
                NSString *name = remaining[index];
                if (record.secure && ([name containsString:@"Text"] || [name containsString:@"Character"] || [name isEqual:@"AXValue"])) continue;
                id raw = GuestAccessibilityValueWithoutError(extraValues[index]);
                id encoded = raw ? [self encodeValue:raw applicationPID:pid] : nil;
                if (encoded) nativeValues[name] = encoded;
            }
            node[@"attributes"] = nativeValues;
            NSDictionary *attributedTitle = nativeValues[@"AXAttributedTitle"];
            if (!title.length && [attributedTitle isKindOfClass:NSDictionary.class] &&
                [attributedTitle[@"$ax"] isEqual:@"attributed"])
                title = GuestAccessibilityString(attributedTitle[@"value"]);
            NSDictionary *attributedDescription = nativeValues[@"AXAttributedDescription"];
            if (!label.length && [attributedDescription isKindOfClass:NSDictionary.class] &&
                [attributedDescription[@"$ax"] isEqual:@"attributed"])
                label = GuestAccessibilityString(attributedDescription[@"value"]);
            if (title) node[@"title"] = title;
            if (label) node[@"label"] = label;
            if (!record.actions || !record.attributeNames || !record.parameterizedNames || !record.writableAttributes) truncated = YES;
            estimatedBytes += 1536 + title.length * 3 + label.length * 3 + children.count * 32;
            if ([node[@"value"] isKindOfClass:NSString.class]) estimatedBytes += [node[@"value"] length] * 3;
            [nodes addObject:node];
        }
    }
    for (NSMutableDictionary *node in nodes) {
        GuestAccessibilityRecord *record = self.records[node[@"id"]];
        node[@"actions"] = record.actions ?: @[];
        node[@"attributeNames"] = record.attributeNames ?: @[];
        node[@"parameterizedNames"] = record.secure ? @[] : (record.parameterizedNames ?: @[]);
        node[@"writableAttributes"] = record.writableAttributes ?: @[];
        node[@"settable"] = record.settable ?: @{};
        node[@"actionDescriptions"] = record.actionDescriptions ?: @{};
        if (record.actionNamesError != kAXErrorSuccess) node[@"actionNamesError"] = @(record.actionNamesError);
        node[@"writableValue"] = @([record.writableAttributes containsObject:@"AXValue"]);
        node[@"writableFocused"] = @([record.writableAttributes containsObject:@"AXFocused"]);
        node[@"writableSelectedTextRange"] = @(!record.secure && [record.writableAttributes containsObject:@"AXSelectedTextRange"]);
    }
    NSMutableDictionary *snapshot = [@{@"type": @"snapshot", @"version": @(ASB_AX_VERSION),
        @"session": self.session, @"revision": @(self.revision), @"app": context[@"app"],
        @"display": context[@"display"], @"roots": roots, @"nodes": nodes, @"truncated": @(truncated),
        @"refreshGeneration": @(self.refreshGeneration)} mutableCopy];
    for (;;) {
        NSMutableSet *emitted = NSMutableSet.set;
        for (NSDictionary *node in nodes) [emitted addObject:node[@"id"]];
        [roots filterUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSString *identifier, NSDictionary *bindings) {
            (void)bindings;
            return [emitted containsObject:identifier];
        }]];
        for (NSMutableDictionary *node in nodes) {
            node[@"children"] = [node[@"children"] filteredArrayUsingPredicate:
                [NSPredicate predicateWithBlock:^BOOL(NSString *identifier, NSDictionary *bindings) {
                    (void)bindings;
                    return [emitted containsObject:identifier];
                }]];
        }
        if (focused && [emitted containsObject:focused]) snapshot[@"focused"] = focused;
        else [snapshot removeObjectForKey:@"focused"];
        NSData *encoded = [NSJSONSerialization dataWithJSONObject:snapshot options:0 error:nil];
        if (encoded && encoded.length <= ASB_AX_MAX_MESSAGE) {
            [self expireRecords];
            return snapshot;
        }
        if (!nodes.count) return nil;
        [nodes removeObjectsInRange:NSMakeRange(nodes.count / 2, nodes.count - nodes.count / 2)];
        snapshot[@"truncated"] = @YES;
    }
}

- (BOOL)rejectAction:(NSDictionary *)message error:(AXError)error {
    return [self send:@{@"type": @"actionResult", @"version": @(ASB_AX_VERSION),
        @"session": self.session, @"requestId": message[@"requestId"], @"ok": @NO, @"error": @(error)}];
}

- (BOOL)expirePendingActions {
    double now = GuestAccessibilityMonotonicTime();
    for (NSString *identifier in self.pendingActions.allKeys) {
        GuestAccessibilityPendingAction *pending = self.pendingActions[identifier];
        if (now - pending.preparedAt < ASB_AX_REQUEST_TIMEOUT_MS / 1000.0) continue;
        [self.pendingActions removeObjectForKey:identifier];
        if (![self rejectAction:pending.message error:kAXErrorCannotComplete]) return NO;
    }
    return YES;
}

- (BOOL)prepareAction:(NSDictionary *)message {
    NSString *request = message[@"requestId"];
    if (![request isKindOfClass:NSString.class] || !request.length || request.length > 128) return NO;
    if (![message[@"session"] isEqual:self.session]) return [self rejectAction:message error:kAXErrorInvalidUIElement];
    if (self.pendingActions[request]) return NO;
    if (self.pendingActions.count >= ASB_AX_MAX_PENDING_REQUESTS)
        return [self rejectAction:message error:kAXErrorCannotComplete];
    GuestAccessibilityPendingAction *pending = [GuestAccessibilityPendingAction new];
    pending.message = message;
    pending.preparedAt = GuestAccessibilityMonotonicTime();
    self.pendingActions[request] = pending;
    return [self send:@{@"type": @"actionReady", @"version": @(ASB_AX_VERSION),
        @"session": self.session, @"requestId": request}];
}

- (BOOL)commitAction:(NSDictionary *)message {
    NSString *request = message[@"requestId"];
    id budget = message[@"remainingMs"];
    if (![request isKindOfClass:NSString.class] || !request.length || request.length > 128 ||
        ![message[@"session"] isEqual:self.session] || ![budget isKindOfClass:NSNumber.class] ||
        !isfinite([budget doubleValue]) || [budget doubleValue] < 0 || [budget doubleValue] > ASB_AX_REQUEST_TIMEOUT_MS) return NO;
    GuestAccessibilityPendingAction *pending = self.pendingActions[request];
    if (!pending) return YES;
    [self.pendingActions removeObjectForKey:request];
    double deadline = pending.preparedAt + [budget doubleValue] / 1000.0;
    return [self handleAction:pending.message deadline:deadline];
}

- (BOOL)prepareNativeCall:(AXUIElementRef)element deadline:(double)deadline {
    double remaining = deadline - GuestAccessibilityMonotonicTime();
    if (remaining <= 0) return NO;
    AXUIElementSetMessagingTimeout(element, (float)MIN(1.0, remaining));
    return YES;
}

- (BOOL)handleAction:(NSDictionary *)message deadline:(double)deadline {
    NSString *request = message[@"requestId"];
    if (![request isKindOfClass:NSString.class] || !request.length || request.length > 128) return NO;
    if (GuestAccessibilityMonotonicTime() >= deadline)
        return [self rejectAction:message error:kAXErrorCannotComplete];
    AXError error = kAXErrorIllegalArgument;
    NSDictionary *context = [self context];
    NSString *identifier = message[@"nodeId"], *action = message[@"action"];
    GuestAccessibilityRecord *record = [identifier isKindOfClass:NSString.class] ? self.records[identifier] : nil;
    BOOL validSession = [message[@"session"] isKindOfClass:NSString.class] && [message[@"session"] isEqualToString:self.session];
    id result = nil;
    NSDictionary *metadata = nil;
    BOOL mutation = NO;
    if (![context[@"trusted"] boolValue]) error = kAXErrorAPIDisabled;
    else if (!self.subscribed || ![context[@"active"] boolValue] || !validSession) error = kAXErrorIllegalArgument;
    else if (!record || record.applicationPID != [context[@"app"][@"pid"] intValue]) error = kAXErrorInvalidUIElement;
    else if (![self prepareNativeCall:record.element deadline:deadline]) error = kAXErrorCannotComplete;
    else if ([action isKindOfClass:NSString.class]) {
        NSString *attribute = message[@"attribute"];
        id encoded = message[@"value"];
        if ([action isEqual:@"setValue"]) { action = @"setAttribute"; attribute = @"AXValue"; }
        if ([action isEqual:@"setFocused"]) { action = @"setAttribute"; attribute = @"AXFocused"; }
        if ([action isEqual:@"setSelectedTextRange"]) { action = @"setAttribute"; attribute = @"AXSelectedTextRange";
            encoded = @{@"$ax": @"range", @"value": encoded ?: NSNull.null}; }
        if ([action isEqual:@"hitTest"]) {
            id value = [self decodeValue:encoded applicationPID:record.applicationPID];
            CGPoint point;
            if (value && CFGetTypeID((__bridge CFTypeRef)value) == AXValueGetTypeID() &&
                AXValueGetValue((__bridge AXValueRef)value, kAXValueCGPointType, &point)) {
                AXUIElementRef application = AXUIElementCreateApplication(record.applicationPID), hit = NULL;
                error = [self prepareNativeCall:application deadline:deadline] ?
                    AXUIElementCopyElementAtPosition(application, point.x, point.y, &hit) : kAXErrorCannotComplete;
                CFRelease(application);
                if (error == kAXErrorSuccess && hit) {
                    result = [self encodeValue:(__bridge id)hit applicationPID:record.applicationPID];
                }
                if (hit) CFRelease(hit);
            }
        } else if ([action isEqual:@"getFrame"]) {
            CFArrayRef values = NULL;
            error = AXUIElementCopyMultipleAttributeValues(record.element,
                (__bridge CFArrayRef)@[@"AXPosition", @"AXSize"], kAXCopyMultipleAttributeOptionStopOnError, &values);
            if (error == kAXErrorSuccess) {
                NSArray *items = (__bridge NSArray *)values;
                CGPoint point;
                CGSize size;
                if (items.count == 2 && CFGetTypeID((__bridge CFTypeRef)items[0]) == AXValueGetTypeID() &&
                    CFGetTypeID((__bridge CFTypeRef)items[1]) == AXValueGetTypeID() &&
                    AXValueGetValue((__bridge AXValueRef)items[0], kAXValueCGPointType, &point) &&
                    AXValueGetValue((__bridge AXValueRef)items[1], kAXValueCGSizeType, &size))
                    result = @[@(point.x), @(point.y), @(size.width), @(size.height)];
                else error = kAXErrorNoValue;
            }
            if (values) CFRelease(values);
        } else if ([action isEqual:@"getMetadata"]) {
            metadata = [self metadataForRecord:record];
            error = metadata ? kAXErrorSuccess : kAXErrorCannotComplete;
        } else if ([action isEqual:@"isAttributeSettable"] && [attribute isKindOfClass:NSString.class] && attribute.length <= 256) {
            Boolean settable = false;
            error = AXUIElementIsAttributeSettable(record.element, (__bridge CFStringRef)attribute, &settable);
            if (error == kAXErrorSuccess) result = @(settable);
        } else if ([action isEqual:@"setAttribute"] && [attribute isKindOfClass:NSString.class] && attribute.length <= 256) {
            Boolean settable = false;
            error = AXUIElementIsAttributeSettable(record.element, (__bridge CFStringRef)attribute, &settable);
            id value = [self decodeValue:encoded applicationPID:record.applicationPID];
            if (error == kAXErrorSuccess && !settable) error = kAXErrorAttributeUnsupported;
            if (error == kAXErrorSuccess && !value) error = kAXErrorIllegalArgument;
            if (error == kAXErrorSuccess && ![self prepareNativeCall:record.element deadline:deadline]) error = kAXErrorCannotComplete;
            if (error == kAXErrorSuccess) {
                mutation = YES;
                error = AXUIElementSetAttributeValue(record.element, (__bridge CFStringRef)attribute, (__bridge CFTypeRef)value);
            }
        } else if (([action isEqual:@"getAttribute"] || [action isEqual:@"getParameterizedAttribute"]) &&
                   [attribute isKindOfClass:NSString.class] && attribute.length <= 256) {
            CFTypeRef value = NULL;
            if (record.secure && ([attribute containsString:@"Text"] || [attribute containsString:@"Character"] || [attribute isEqual:@"AXValue"]))
                error = kAXErrorAttributeUnsupported;
            else if (![self prepareNativeCall:record.element deadline:deadline]) error = kAXErrorCannotComplete;
            else if ([action isEqual:@"getAttribute"]) {
                mutation = AsbAXParameterizedAttributeMutates(attribute);
                error = AXUIElementCopyAttributeValue(record.element, (__bridge CFStringRef)attribute, &value);
            } else {
                id parameter = [self decodeValue:encoded applicationPID:record.applicationPID];
                if (!parameter) error = kAXErrorIllegalArgument;
                else if (![self prepareNativeCall:record.element deadline:deadline]) error = kAXErrorCannotComplete;
                else {
                    mutation = AsbAXParameterizedAttributeMutates(attribute);
                    error = AXUIElementCopyParameterizedAttributeValue(record.element,
                        (__bridge CFStringRef)attribute, (__bridge CFTypeRef)parameter, &value);
                }
            }
            if (error == kAXErrorSuccess) {
                result = [self encodeValue:(__bridge id)value applicationPID:record.applicationPID];
                if (!result) error = kAXErrorNotImplemented;
            }
            if (value) CFRelease(value);
        } else if (action.length && action.length <= 256) {
            CFArrayRef actions = NULL;
            error = AXUIElementCopyActionNames(record.element, &actions);
            if (error == kAXErrorSuccess && ![(__bridge NSArray *)actions containsObject:action]) error = kAXErrorActionUnsupported;
            if (actions) CFRelease(actions);
            if (error == kAXErrorSuccess && ![self prepareNativeCall:record.element deadline:deadline]) error = kAXErrorCannotComplete;
            if (error == kAXErrorSuccess) {
                mutation = YES;
                error = AXUIElementPerformAction(record.element, (__bridge CFStringRef)action);
            }
        }
    }
    if (mutation) {
        self.workGeneration++;
        self.captureFailures = 0;
        self.captureRequested = NO;
        self.nextSnapshot = INFINITY;
    }
    NSMutableDictionary *reply = [@{@"type": @"actionResult", @"version": @(ASB_AX_VERSION),
        @"session": self.session, @"requestId": request, @"ok": @(error == kAXErrorSuccess), @"error": @(error)} mutableCopy];
    if (result) reply[@"value"] = result;
    if (metadata) reply[@"record"] = metadata;
    return [self send:reply];
}

- (BOOL)handleMessage:(NSDictionary *)message {
    id version = message[@"version"];
    if (version && (![version isKindOfClass:NSNumber.class] || [version integerValue] != ASB_AX_VERSION)) return NO;
    if ([message[@"type"] isEqual:@"subscribe"]) {
        if (![message[@"enabled"] isKindOfClass:NSNumber.class]) return NO;
        self.subscribed = [message[@"enabled"] boolValue];
        self.workGeneration++;
        self.captureFailures = 0;
        self.captureRequested = self.subscribed;
        self.backgroundCaptureRequested = NO;
        self.nextSnapshot = GuestAccessibilityMonotonicTime() + ASB_AX_INPUT_SETTLE_MS / 1000.0;
        if (self.subscribed && ![[self context][@"active"] boolValue]) {
            [self sendStatus:@"inactive"];
            return NO;
        }
        if (self.subscribed && !self.permissionRequested) {
            self.permissionRequested = YES;
            dispatch_async(dispatch_get_main_queue(), ^{
                NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
                AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
            });
        }
        if (!self.subscribed) {
            [self.pendingActions removeAllObjects];
            [self prune:NSSet.set];
            return [self sendStatus:@"inactive"];
        }
        return YES;
    }
    if ([message[@"type"] isEqual:@"action"]) {
        id confirm = message[@"confirmBeforeExecution"];
        if (confirm && (![confirm isKindOfClass:NSNumber.class] ||
            ([confirm doubleValue] != 0 && [confirm doubleValue] != 1))) return NO;
        return [confirm boolValue] ? [self prepareAction:message] :
            [self handleAction:message deadline:GuestAccessibilityMonotonicTime() + ASB_AX_REQUEST_TIMEOUT_MS / 1000.0];
    }
    if ([message[@"type"] isEqual:@"actionCommit"]) return [self commitAction:message];
    if ([message[@"type"] isEqual:@"refresh"]) {
        id generation = message[@"generation"];
        if (![generation isKindOfClass:NSNumber.class] || !isfinite([generation doubleValue]) ||
            [generation doubleValue] < 0 || [generation doubleValue] > 9007199254740991.0 ||
            floor([generation doubleValue]) != [generation doubleValue]) return NO;
        uint64_t requested = [generation unsignedLongLongValue];
        if (requested < self.refreshGeneration) return YES;
        if (requested == self.refreshGeneration && (self.captureRequested || self.captureDeadline > 0)) return YES;
        self.refreshGeneration = requested;
        self.workGeneration++;
        self.captureFailures = 0;
        self.captureRequested = self.subscribed;
        self.backgroundCaptureRequested = NO;
        @synchronized(self) { self.nextSnapshot = _systemInputAt + ASB_AX_INPUT_SETTLE_MS / 1000.0; }
        return YES;
    }
    return NO;
}

- (void)run {
    while (!g_stop) {
        @autoreleasepool {
            if (![[self context][@"active"] boolValue]) {
                for (int i = 0; i < 20 && !g_stop; i++) usleep(100000);
                continue;
            }
            self.socket = [self connect];
            if (self.socket < 0) {
                for (int i = 0; i < 20 && !g_stop; i++) usleep(100000);
                continue;
            }
            self.session = NSUUID.UUID.UUIDString;
            self.subscribed = NO;
            self.captureRequested = NO;
            self.backgroundCaptureRequested = NO;
            self.backgroundCaptureAfter = 0;
            self.captureFailures = 0;
            self.connectionFailed = NO;
            self.captureDeadline = 0;
            self.revision = 0;
            self.publishedRevision = 0;
            self.refreshGeneration = 0;
            self.workGeneration = 0;
            self.nextSnapshot = INFINITY;
            self.nextHeartbeat = GuestAccessibilityMonotonicTime() + 1.0;
            self.publishedRecords = nil;
            self.capturingRecords = nil;
            [self.pendingActions removeAllObjects];
            [self prune:NSSet.set];
            BOOL connected = [self sendStatus:@"inactive"];
            while (connected && !g_stop && !self.connectionFailed) {
                @autoreleasepool {
                    [self processSystemInput];
                    if (![self expirePendingActions]) self.connectionFailed = YES;
                    if (self.connectionFailed) break;
                    double now = GuestAccessibilityMonotonicTime();
                    double wait = MIN(0.1, MAX(0, self.nextHeartbeat - now));
                    if (self.captureRequested) wait = MIN(wait, MAX(0, self.nextSnapshot - now));
                    fd_set reads;
                    FD_ZERO(&reads);
                    FD_SET(self.socket, &reads);
                    struct timeval timeout = { .tv_sec = 0, .tv_usec = (suseconds_t)(wait * 1000000) };
                    int ready = select(self.socket + 1, &reads, NULL, NULL, &timeout);
                    if (ready < 0 && errno == EINTR) continue;
                    if (ready < 0) break;
                    if (ready > 0) {
                        NSDictionary *message = AsbAXReadMessage(self.socket);
                        if (!message || ![self handleMessage:message]) break;
                        continue;
                    }
                    if (self.subscribed && self.captureRequested && GuestAccessibilityMonotonicTime() >= self.nextSnapshot) {
                        self.captureRequested = NO;
                        self.backgroundCaptureRequested = NO;
                        self.nextSnapshot = INFINITY;
                        self.collectingGeneration = self.workGeneration;
                        double captureStarted = GuestAccessibilityMonotonicTime();
                        self.captureDeadline = captureStarted + ASB_AX_CAPTURE_TIMEOUT_MS / 1000.0;
                        self.capturingRecords = NSMutableSet.set;
                        NSDictionary *context = [self context];
                        if (![context[@"active"] boolValue]) {
                            [self prune:NSSet.set];
                            [self sendStatus:@"inactive"];
                            connected = NO;
                        } else if (![context[@"trusted"] boolValue]) {
                            [self prune:NSSet.set];
                            connected = [self sendStatus:@"permissionRequired"];
                        } else if ([context[@"app"][@"pid"] intValue] <= 0) {
                            [self prune:NSSet.set];
                            connected = [self sendStatus:@"inactive"];
                        } else if ((connected = [self sendStatus:@"updating"])) {
                            NSDictionary *snapshot = [self snapshot:context];
                            if (self.connectionFailed) connected = NO;
                            else if (self.collectingGeneration == self.workGeneration && self.subscribed) {
                                NSDictionary *after = [self context];
                                [self processSystemInput];
                                if (self.collectingGeneration == self.workGeneration) {
                                    BOOL complete = snapshot && ![snapshot[@"truncated"] boolValue] &&
                                        GuestAccessibilityMonotonicTime() <= self.captureDeadline && [after[@"active"] boolValue] &&
                                        [after[@"app"][@"pid"] isEqual:context[@"app"][@"pid"]];
                                    connected = complete ? [self send:snapshot] : [self sendStatus:@"captureFailed"];
                                    if (complete) {
                                        self.captureFailures = 0;
                                        if (connected) {
                                            self.publishedRecords = self.capturingRecords;
                                            self.publishedRevision = self.revision;
                                        }
                                    }
                                    else if (connected && ++self.captureFailures <= 2 && ![snapshot[@"truncated"] boolValue] &&
                                             [after[@"active"] boolValue] && [after[@"trusted"] boolValue] &&
                                             [after[@"app"][@"pid"] isEqual:context[@"app"][@"pid"]]) {
                                        self.captureRequested = YES;
                                        self.backgroundCaptureRequested = NO;
                                        self.nextSnapshot = GuestAccessibilityMonotonicTime() + 0.5 * self.captureFailures;
                                    }
                                }
                            }
                        }
                        self.captureDeadline = 0;
                        self.capturingRecords = nil;
                        [self expireRecords];
                        double finished = GuestAccessibilityMonotonicTime();
                        self.backgroundCaptureAfter = finished + MAX(1.0, 3 * (finished - captureStarted));
                        if (self.captureRequested && self.backgroundCaptureRequested)
                            self.nextSnapshot = MAX(self.nextSnapshot, self.backgroundCaptureAfter);
                        self.nextHeartbeat = GuestAccessibilityMonotonicTime() + 1.0;
                    } else if (GuestAccessibilityMonotonicTime() >= self.nextHeartbeat) {
                        NSDictionary *context = [self context];
                        if (![context[@"active"] boolValue]) {
                            [self prune:NSSet.set];
                            [self sendStatus:@"inactive"];
                            connected = NO;
                        } else connected = [self sendStatus:self.subscribed ? @"ready" : @"inactive"];
                        self.nextHeartbeat = GuestAccessibilityMonotonicTime() + 1.0;
                    }
                }
            }
            shutdown(self.socket, SHUT_RDWR);
            close(self.socket);
            self.socket = -1;
            [self prune:NSSet.set];
            for (int i = 0; i < 20 && !g_stop; i++) usleep(100000);
        }
    }
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

@end

int main(void) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);
        signal(SIGTERM, GuestAccessibilityStopSignal);
        signal(SIGINT, GuestAccessibilityStopSignal);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        GuestAccessibilityCollector *exporter = [GuestAccessibilityCollector new];
        [exporter observeSystemInput];
        dispatch_queue_t queue = dispatch_queue_create("com.appsandbox.accessibility", DISPATCH_QUEUE_SERIAL);
        dispatch_async(queue, ^{ [exporter run]; });
        [NSApp run];
    }
    return 0;
}
