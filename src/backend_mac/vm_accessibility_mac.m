#import "vm_accessibility_mac.h"
#import "asb_ivshmem_transport.h"
#import "vm_accessibility_session.h"
#import "../../tools/transport/accessibility_protocol.h"
#import "../../tools/transport/asb_transport.h"
#include <unistd.h>

@interface VmAccessibilityMac () <VZVirtioSocketListenerDelegate>
- (void)connectTransport;
- (void)displayChanged:(VZGraphicsDisplay *)display available:(BOOL)available;
@end

@interface VmAccessibilityDisplayObserver : NSObject <VZGraphicsDisplayObserver>
@property(nonatomic, weak) VmAccessibilityMac *owner;
@end
@implementation VmAccessibilityDisplayObserver
- (void)displayDidBeginReconfiguration:(VZGraphicsDisplay *)display { [self.owner displayChanged:display available:NO]; }
- (void)displayDidEndReconfiguration:(VZGraphicsDisplay *)display { [self.owner displayChanged:display available:YES]; }
@end

@implementation VmAccessibilityMac {
    VmAccessibilitySession *_session;
    __weak VZVirtualMachine *_virtualMachine;
    VZVirtioSocketDevice *_socketDevice;
    VZVirtioSocketListener *_listener;
    VZVirtioSocketConnection *_socketConnection;
    VZGraphicsDisplay *_graphicsDisplay;
    VmAccessibilityDisplayObserver *_displayObserver;
    AsbIvshmemTransport *_transport;
    NSTimer *_connectTimer;
    BOOL _started;
    BOOL _connecting;
    NSUInteger _generation;
}
- (instancetype)initWithView:(NSView *)view vmName:(NSString *)name {
    if ((self = [super init])) {
        _session = [[VmAccessibilitySession alloc] initWithView:view name:name];
    }
    return self;
}
- (void)dealloc {
    [_connectTimer invalidate];
    [_session stop];
    _displayObserver.owner = nil;
    if (_displayObserver) [_graphicsDisplay removeObserver:_displayObserver];
    if (_listener) [_socketDevice removeSocketListenerForPort:ASB_AX_PORT];
    [_socketConnection close];
}
- (void)configureWithSocketDevice:(VZVirtioSocketDevice *)device virtualMachine:(VZVirtualMachine *)machine {
    [self stopAccessibility];
    _transport = nil;
    _socketDevice = device;
    _virtualMachine = machine;
}
- (void)configureWithTransport:(AsbIvshmemTransport *)transport {
    [self stopAccessibility];
    _socketDevice = nil;
    _virtualMachine = nil;
    _transport = transport;
}
- (void)startAccessibility {
    if (_started || (!_socketDevice && !_transport)) return;
    _started = YES;
    _generation++;
    [_session start];
    if (_socketDevice) {
        _listener = [VZVirtioSocketListener new];
        _listener.delegate = self;
        [_socketDevice setSocketListener:_listener forPort:ASB_AX_PORT];
        _graphicsDisplay = _virtualMachine.graphicsDevices.firstObject.displays.firstObject;
        if (_graphicsDisplay) {
            _displayObserver = [VmAccessibilityDisplayObserver new];
            _displayObserver.owner = self;
            [_graphicsDisplay addObserver:_displayObserver];
            [_session setDisplayPixels:_graphicsDisplay.sizeInPixels available:YES];
        }
    }
    if (_transport) {
        __weak VmAccessibilityMac *weakSelf = self;
        _connectTimer = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) { [weakSelf connectTransport]; }];
        [NSRunLoop.mainRunLoop addTimer:_connectTimer forMode:NSRunLoopCommonModes];
        [self connectTransport];
    }
}
- (void)stopAccessibility {
    _started = NO;
    _generation++;
    _connecting = NO;
    [_connectTimer invalidate];
    _connectTimer = nil;
    [_session stop];
    if (_listener) [_socketDevice removeSocketListenerForPort:ASB_AX_PORT];
    _listener.delegate = nil;
    _listener = nil;
    [_socketConnection close];
    _socketConnection = nil;
    _displayObserver.owner = nil;
    if (_displayObserver) [_graphicsDisplay removeObserver:_displayObserver];
    _displayObserver = nil;
    _graphicsDisplay = nil;
}
- (void)connectTransport {
    if (!_started || !_transport || _session.connected || _connecting) return;
    _connecting = YES;
    NSUInteger generation = _generation;
    AsbIvshmemTransport *transport = _transport;
    __weak VmAccessibilityMac *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int fd = [transport connectChannel:ASB_CH_ACCESSIBILITY timeoutMs:500];
        dispatch_async(dispatch_get_main_queue(), ^{
            VmAccessibilityMac *bridge = weakSelf;
            if (bridge && bridge->_started && bridge->_generation == generation) {
                bridge->_connecting = NO;
                if (fd >= 0) [bridge->_session attachFileDescriptor:fd];
            }
            if (fd >= 0) close(fd);
        });
    });
}
- (BOOL)listener:(VZVirtioSocketListener *)listener shouldAcceptNewConnection:(VZVirtioSocketConnection *)connection
        fromSocketDevice:(VZVirtioSocketDevice *)socketDevice {
    if (!_started || listener != _listener || socketDevice != _socketDevice) return NO;
    if (![_session attachFileDescriptor:connection.fileDescriptor]) return NO;
    [_socketConnection close];
    _socketConnection = connection;
    return YES;
}
- (void)displayChanged:(VZGraphicsDisplay *)display available:(BOOL)available {
    if (!_started || display != _graphicsDisplay) return;
    [_session setDisplayPixels:display.sizeInPixels available:available];
}
- (void)updateDisplayPixels:(NSSize)pixels { [_session setDisplayPixels:pixels available:YES]; }
- (void)observeWindow { [_session observeWindow]; }
- (void)geometryDidChange { [_session geometryDidChange]; }
- (BOOL)hasAccessibilityContent { return _session.hasAccessibilityContent; }
- (NSString *)accessibilityLabel { return _session.accessibilityLabel; }
- (NSArray *)accessibilityChildren { return _session.accessibilityChildren; }
- (id)accessibilityFocusedUIElement { return _session.accessibilityFocusedUIElement; }
- (id)accessibilityHitTest:(NSPoint)point { return [_session accessibilityHitTest:point]; }
@end
