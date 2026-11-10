#include "asb_events.h"
#import "vz_display.h"
#import "vz_vm.h"
#import "asb_core_mac.h"
#import "agent_overlay.h"
#include "headless_vm.h"
#include "input_keys.h"
#include <IOKit/hidsystem/IOLLEvent.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

@interface AsbOffscreenVmWindow:NSWindow @end
@implementation AsbOffscreenVmWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
- (void)sendEvent:(NSEvent *)event { (void)event; }
@end

@interface VzDisplayWindow () <NSWindowDelegate>
@property (nonatomic, strong) VzVm *vm;
@property (nonatomic, strong) VZVirtualMachineView *vmView;
- (BOOL)dispatchInputPacket:(InputPacket)packet token:(id)token;
- (void)releaseNativeInput:(id)token;
@end

int asb_mac_native_input_connect(const AsbMacHeadlessTarget *target)
{
    int pair[2];if (socketpair(AF_UNIX,SOCK_STREAM,0,pair)) return -1;
    for (unsigned i=0;i<2;++i) {
        int yes=1;setsockopt(pair[i],SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));fcntl(pair[i],F_SETFD,FD_CLOEXEC);
    }
    uint64_t identity=target->id,run=target->run_id;int fd=pair[1];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{ @autoreleasepool {
        NSObject *token=[NSObject new];
        __block VzDisplayWindow *recipient=nil;
        uint32_t ready=INPUT_READY_MAGIC;
        BOOL alive=!asb_mac_channel_write(fd,&ready,sizeof(ready));
        InputPacket packet;size_t used=0;
        while (alive) {
            struct pollfd pending={fd,POLLIN,0};int result=poll(&pending,1,1000);
            if (result<0 && errno==EINTR) continue;
            if (result<0) break;if (!result) continue;
            ssize_t n=recv(fd,(char *)&packet+used,sizeof(packet)-used,0);
            if (n<0 && errno==EINTR) continue;if (n<=0) break;
            used+=(size_t)n;if (used<sizeof(packet)) continue;used=0;
            if (packet.magic!=INPUT_MAGIC) break;
            __block BOOL sent=NO;
            dispatch_sync(dispatch_get_main_queue(),^{
                AsbVmMac *vm=asb_mac_vm_find_by_id(identity);
                if (!vm || vm->run_id!=run || !vm->running || vm->shutting_down || !vm->vz_handle) return;
                if (!recipient) {
                    if ([vm->display isKindOfClass:VzDisplayWindow.class] && !vm->display.userClosed)
                        recipient=vm->display;
                    else recipient=[[VzDisplayWindow alloc] initWithVzVm:vm->vz_handle vmId:identity offscreen:YES];
                }
                sent=[recipient dispatchInputPacket:packet token:token];
            });
            if (!sent) break;
            InputPacket reply={INPUT_MAGIC,0,0,packet.param2,0};
            if (packet.type==INPUT_KEYBOARD_QUERY) { reply.type=INPUT_KEYBOARD_REPLY;reply.param1=INPUT_KEYBOARD_VERSION; }
            else if (packet.type==INPUT_MOUSE_QUERY) { reply.type=INPUT_MOUSE_REPLY;reply.param1=INPUT_MOUSE_NORMALIZED_VERSION; }
            else if (packet.type==INPUT_MOUSE_POSITION_QUERY) {
                reply.type=INPUT_MOUSE_POSITION_REPLY;reply.param1=reply.param2=INT32_MIN;reply.param3=packet.param1;
            }
            if (reply.type) alive=!asb_mac_channel_write(fd,&reply,sizeof(reply));
        }
        close(fd);
        dispatch_async(dispatch_get_main_queue(),^{ [recipient releaseNativeInput:token]; });
    }});
    fcntl(pair[0],F_SETFL,fcntl(pair[0],F_GETFL)|O_NONBLOCK);return pair[0];
}

@implementation VzDisplayWindow {
    AsbAgentOverlay *_overlay;
    id _inputMonitor;
    NSTimer *_inputTimer;
    NSMutableDictionary<NSNumber *,NSEvent *> *_heldKeys;
    NSEvent *_heldButtons[3];
    NSMutableSet<NSNumber *> *_modifierKeys;
    unsigned _preparingInput;
    BOOL _nativeKeys[128];
    unsigned _nativeButtons;
    NSPoint _nativePoint;
    id _nativeInputToken;
    BOOL _offscreen;
}

- (NSEventModifierFlags)nativeModifiers {
    NSEventModifierFlags flags=0;
    const struct { unsigned key;NSEventModifierFlags flag,side; } modifiers[]={
        {kVK_Shift,NSEventModifierFlagShift,NX_DEVICELSHIFTKEYMASK},
        {kVK_RightShift,NSEventModifierFlagShift,NX_DEVICERSHIFTKEYMASK},
        {kVK_Control,NSEventModifierFlagControl,NX_DEVICELCTLKEYMASK},
        {kVK_RightControl,NSEventModifierFlagControl,NX_DEVICERCTLKEYMASK},
        {kVK_Option,NSEventModifierFlagOption,NX_DEVICELALTKEYMASK},
        {kVK_RightOption,NSEventModifierFlagOption,NX_DEVICERALTKEYMASK},
        {kVK_Command,NSEventModifierFlagCommand,NX_DEVICELCMDKEYMASK},
        {kVK_RightCommand,NSEventModifierFlagCommand,NX_DEVICERCMDKEYMASK},
        {kVK_CapsLock,NSEventModifierFlagCapsLock,0}
    };
    for (unsigned i=0;i<sizeof(modifiers)/sizeof(modifiers[0]);++i)
        if (_nativeKeys[modifiers[i].key]) flags|=modifiers[i].flag|modifiers[i].side;
    return flags;
}

- (void)nativeKey:(unsigned)key down:(BOOL)down {
    BOOL repeat=down && _nativeKeys[key];_nativeKeys[key]=down;
    BOOL modifier=key==kVK_Command || key==kVK_RightCommand || key==kVK_Shift || key==kVK_RightShift ||
        key==kVK_Control || key==kVK_RightControl || key==kVK_Option || key==kVK_RightOption || key==kVK_CapsLock;
    NSEvent *event=[NSEvent keyEventWithType:modifier?NSEventTypeFlagsChanged:down?NSEventTypeKeyDown:NSEventTypeKeyUp
        location:_nativePoint modifierFlags:self.nativeModifiers timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:self.window.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@""
        isARepeat:repeat keyCode:(unsigned short)key];
    if (modifier) [self.vmView flagsChanged:event];
    else if (down) [self.vmView keyDown:event];else [self.vmView keyUp:event];
}

- (NSEvent *)nativeMouseEvent:(NSEventType)type button:(unsigned)button {
    BOOL down=type==NSEventTypeLeftMouseDown || type==NSEventTypeRightMouseDown || type==NSEventTypeOtherMouseDown;
    BOOL up=type==NSEventTypeLeftMouseUp || type==NSEventTypeRightMouseUp || type==NSEventTypeOtherMouseUp;
    NSEvent *event=[NSEvent mouseEventWithType:type location:_nativePoint modifierFlags:self.nativeModifiers
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:self.window.windowNumber context:nil
        eventNumber:0 clickCount:down || up?1:0 pressure:down?1:0];
    if (!event) return nil;
    CGEventRef original=event.CGEvent;
    CGEventRef mouse=original?CGEventCreateCopy(original):NULL;
    if (!mouse) return nil;
    CGEventSetIntegerValueField(mouse,kCGMouseEventButtonNumber,button);
    event=[NSEvent eventWithCGEvent:mouse];CFRelease(mouse);
    return event;
}

- (BOOL)nativeButton:(unsigned)button down:(BOOL)down {
    NSEventType type=button==0?(down?NSEventTypeLeftMouseDown:NSEventTypeLeftMouseUp):
        button==1?(down?NSEventTypeRightMouseDown:NSEventTypeRightMouseUp):(down?NSEventTypeOtherMouseDown:NSEventTypeOtherMouseUp);
    NSEvent *event=[self nativeMouseEvent:type button:button];
    if (!event) return NO;
    if (down) _nativeButtons|=1u<<button;else _nativeButtons&=~(1u<<button);
    if (button==0) { if (down) [self.vmView mouseDown:event];else [self.vmView mouseUp:event]; }
    else if (button==1) { if (down) [self.vmView rightMouseDown:event];else [self.vmView rightMouseUp:event]; }
    else { if (down) [self.vmView otherMouseDown:event];else [self.vmView otherMouseUp:event]; }
    return YES;
}

- (void)releaseNativeInput:(id)token {
    if (token && token!=_nativeInputToken) return;
    for (unsigned key=0;key<128;++key) if (_nativeKeys[key]) [self nativeKey:key down:NO];
    for (unsigned button=0;button<3;++button) if (_nativeButtons&(1u<<button)) [self nativeButton:button down:NO];
    _nativeInputToken=nil;
}

- (BOOL)dispatchInputPacket:(InputPacket)packet token:(id)token {
    if (_offscreen || self.userClosed) {
        NSSize size=_vm.machine.graphicsDevices.firstObject.displays.firstObject.sizeInPixels;
        if (size.width<1 || size.height<1 || size.width>7680 || size.height>4320) return NO;
        if (!NSEqualSizes(self.vmView.bounds.size,size)) {
            NSSize previous=self.vmView.bounds.size;
            NSPoint point=[self.vmView convertPoint:_nativePoint fromView:nil];
            [self.window setContentSize:size];
            _nativePoint=[self.vmView convertPoint:NSMakePoint(point.x*size.width/previous.width,
                point.y*size.height/previous.height) toView:nil];
        }
    }
    if (_nativeInputToken!=token) { [self releaseNativeInput:nil];_nativeInputToken=token; }
    if (packet.type==INPUT_KEYBOARD_QUERY || packet.type==INPUT_MOUSE_QUERY || packet.type==INPUT_MOUSE_POSITION_QUERY) return YES;
    if (packet.type==INPUT_KEY_PHYSICAL) {
        int key=asb_mac_key_code(&packet);if (key<0) return NO;
        [self nativeKey:(unsigned)key down:!(packet.param3&INPUT_KEY_UP)];return YES;
    }
    if (packet.type==INPUT_MOUSE_MOVE && packet.param3==INPUT_MOUSE_NORMALIZED && packet.param1<=65535 && packet.param2<=65535) {
        NSRect bounds=self.vmView.bounds;
        double y=packet.param2/65535.0;
        NSPoint point=NSMakePoint(packet.param1/65535.0*NSWidth(bounds),(self.vmView.isFlipped?y:1-y)*NSHeight(bounds));
        _nativePoint=[self.vmView convertPoint:point toView:nil];
        NSEventType type=(_nativeButtons&1)?NSEventTypeLeftMouseDragged:(_nativeButtons&2)?NSEventTypeRightMouseDragged:
            (_nativeButtons&4)?NSEventTypeOtherMouseDragged:NSEventTypeMouseMoved;
        unsigned button=(_nativeButtons&1)?0:(_nativeButtons&2)?1:(_nativeButtons&4)?2:0;
        NSEvent *event=[self nativeMouseEvent:type button:button];
        if (!event) return NO;
        if (type==NSEventTypeLeftMouseDragged) [self.vmView mouseDragged:event];
        else if (type==NSEventTypeRightMouseDragged) [self.vmView rightMouseDragged:event];
        else if (type==NSEventTypeOtherMouseDragged) [self.vmView otherMouseDragged:event];
        else [self.vmView mouseMoved:event];return YES;
    }
    if (packet.type==INPUT_MOUSE_BUTTON && packet.param1<3 && packet.param2<=1) {
        return [self nativeButton:packet.param1 down:packet.param2!=0];
    }
    if (packet.type==INPUT_MOUSE_WHEEL) {
        CGEventRef wheel=CGEventCreateScrollWheelEvent(NULL,kCGScrollEventUnitPixel,1,(int32_t)packet.param1);
        if (!wheel) return NO;
        CGEventSetFlags(wheel,(CGEventFlags)self.nativeModifiers);
        [self.vmView scrollWheel:[NSEvent eventWithCGEvent:wheel]];CFRelease(wheel);return YES;
    }
    return NO;
}

- (BOOL)updateInputControl {
    AsbMacInputStatus status;asb_mac_input_context(self.vmId,&status);
    BOOL allowed=!_offscreen && !self.userClosed && !_preparingInput && (status.flags&ASB_STATUS_USER_HAS_CONTROL);
    self.vmView.capturesSystemKeys=allowed;
    return allowed;
}

- (void)prepareForAgentInput {
    ++_preparingInput;
    [self updateInputControl];
    for (NSEvent *event in _heldKeys.allValues) {
        NSEvent *up=[NSEvent keyEventWithType:NSEventTypeKeyUp location:event.locationInWindow modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:self.window.windowNumber context:nil
            characters:event.characters?:@"" charactersIgnoringModifiers:event.charactersIgnoringModifiers?:@""
            isARepeat:NO keyCode:event.keyCode];
        [self.vmView keyUp:up];
    }
    [_heldKeys removeAllObjects];
    for (NSNumber *key in _modifierKeys) {
        NSEvent *up=[NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:self.window.windowNumber context:nil
            characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:key.unsignedShortValue];
        [self.vmView flagsChanged:up];
    }
    [_modifierKeys removeAllObjects];
    for (unsigned i=0;i<3;++i) if (_heldButtons[i]) {
        NSEvent *event=_heldButtons[i];
        NSEventType type=i==0?NSEventTypeLeftMouseUp:i==1?NSEventTypeRightMouseUp:NSEventTypeOtherMouseUp;
        NSEvent *up=[NSEvent mouseEventWithType:type location:event.locationInWindow modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:self.window.windowNumber context:nil
            eventNumber:0 clickCount:1 pressure:0];
        if (i==0) [self.vmView mouseUp:up];else if (i==1) [self.vmView rightMouseUp:up];else [self.vmView otherMouseUp:up];
        _heldButtons[i]=nil;
    }
}
- (void)finishPreparingForAgentInput {
    if (_preparingInput) --_preparingInput;
    [self updateInputControl];
}
- (NSEvent *)routeViewerInput:(NSEvent *)event {
    if (event.window!=self.window) return event;
    BOOL allowed=[self updateInputControl];
    BOOL key=event.type==NSEventTypeKeyDown || event.type==NSEventTypeKeyUp || event.type==NSEventTypeFlagsChanged;
    if (!key && [_overlay handleViewerMouseEvent:event]) return nil;
    NSPoint point=[self.vmView convertPoint:event.locationInWindow fromView:nil];
    if (!key && !NSPointInRect(point,self.vmView.bounds)) return event;
    if (!allowed) return nil;
    if (event.type==NSEventTypeKeyDown) _heldKeys[@(event.keyCode)]=event;
    else if (event.type==NSEventTypeKeyUp) [_heldKeys removeObjectForKey:@(event.keyCode)];
    else if (event.type==NSEventTypeFlagsChanged) [_modifierKeys addObject:@(event.keyCode)];
    else if (event.type==NSEventTypeLeftMouseDown) _heldButtons[0]=event;
    else if (event.type==NSEventTypeRightMouseDown) _heldButtons[1]=event;
    else if (event.type==NSEventTypeOtherMouseDown) _heldButtons[2]=event;
    else if (event.type==NSEventTypeLeftMouseUp) _heldButtons[0]=nil;
    else if (event.type==NSEventTypeRightMouseUp) _heldButtons[1]=nil;
    else if (event.type==NSEventTypeOtherMouseUp) _heldButtons[2]=nil;
    return event;
}
- (void)stopInputMonitor {
    if (_inputMonitor) [NSEvent removeMonitor:_inputMonitor];_inputMonitor=nil;
    [_inputTimer invalidate];_inputTimer=nil;
    [_overlay invalidate];_overlay=nil;
}
- (void)dealloc { [self stopInputMonitor]; }


- (instancetype)initWithVzVm:(VzVm *)vm vmId:(uint64_t)vmId {
    return [self initWithVzVm:vm vmId:vmId offscreen:NO];
}

- (instancetype)initWithVzVm:(VzVm *)vm vmId:(uint64_t)vmId offscreen:(BOOL)offscreen {
    NSRect frame = NSMakeRect(0, 0, 1280, 800);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    if (offscreen) {
        NSSize size=vm.machine.graphicsDevices.firstObject.displays.firstObject.sizeInPixels;
        if (size.width<1 || size.height<1 || size.width>7680 || size.height>4320) return nil;
        frame.size=size;style=NSWindowStyleMaskBorderless;
    }
    NSWindow *window = [[(offscreen?AsbOffscreenVmWindow.class:AsbViewerWindow.class) alloc] initWithContentRect:frame
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    window.title = [NSString stringWithFormat:@"%@ — Display", vm.name];
    if (offscreen) {
        window.releasedWhenClosed=NO;window.ignoresMouseEvents=YES;
        window.excludedFromWindowsMenu=YES;window.accessibilityElement=NO;
    } else [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _vm = vm;
    _vmId = vmId;
    _offscreen=offscreen;
    self.userClosed=offscreen;
    _vmView = [[VZVirtualMachineView alloc] initWithFrame:frame];
    _vmView.capturesSystemKeys = !offscreen;
    _vmView.automaticallyReconfiguresDisplay = !offscreen;
    _vmView.wantsLayer=YES;
    _vmView.virtualMachine = vm.machine;
    _vmView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = _vmView;
    window.delegate = self;
    return self;
}

- (void)showDisplay {
    if (!_overlay) {
        _heldKeys=[NSMutableDictionary dictionary];
        _modifierKeys=[NSMutableSet set];
        _overlay=[[AsbAgentOverlay alloc] initWithView:self.vmView vmId:self.vmId];
        __weak VzDisplayWindow *weak=self;
        _overlay.prepareForAgentInput=^{ [weak prepareForAgentInput];[weak finishPreparingForAgentInput]; };
        NSEventMask mask=NSEventMaskKeyDown|NSEventMaskKeyUp|NSEventMaskFlagsChanged|NSEventMaskMouseMoved|
            NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp|NSEventMaskRightMouseDown|NSEventMaskRightMouseUp|
            NSEventMaskOtherMouseDown|NSEventMaskOtherMouseUp|NSEventMaskLeftMouseDragged|NSEventMaskRightMouseDragged|
            NSEventMaskOtherMouseDragged|NSEventMaskScrollWheel;
        _inputMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
            VzDisplayWindow *owner=weak;return owner?[owner routeViewerInput:event]:event;
        }];
        _inputTimer=[NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *timer) { (void)timer;[weak updateInputControl]; }];
        [NSRunLoop.mainRunLoop addTimer:_inputTimer forMode:NSRunLoopCommonModes];
    }
    self.userClosed = NO;
    [self updateInputControl];
    asb_events_changed();
    [self showWindow:nil];
    /* Bring the window to the front. The headless daemon is an Accessory app, so
       its window would otherwise open behind the active app; activating brings it
       forward and gives it key focus. Harmless in the GUI (already the active app). */
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];
    /* VZ doesn't surface view-attach events to the guest, so the host
     * pushes mute / clipboard-sync commands to the guest agent on
     * show / focus changes. No-ops if the agent isn't online yet. */
    asb_mac_vm_set_audio_muted(self.vmId, NO);
    asb_mac_vm_set_clipboard_sync(self.vmId, YES);
}

#pragma mark - NSWindowDelegate

- (void)windowWillClose:(NSNotification *)notification {
    self.userClosed=YES;
    self.vmView.automaticallyReconfiguresDisplay=NO;
    [self prepareForAgentInput];[self finishPreparingForAgentInput];
    [self stopInputMonitor];
    if (_offscreen) return;
    asb_mac_display_did_close(self.vmId, self);
    asb_events_changed();
    asb_mac_vm_set_audio_muted(self.vmId, YES);
    asb_mac_vm_set_clipboard_sync(self.vmId, NO);
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    asb_mac_vm_set_audio_muted(self.vmId, YES);
    asb_mac_vm_set_clipboard_sync(self.vmId, NO);
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
    asb_mac_vm_set_audio_muted(self.vmId, NO);
    asb_mac_vm_set_clipboard_sync(self.vmId, YES);
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    asb_mac_vm_set_clipboard_sync(self.vmId, YES);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    [self prepareForAgentInput];[self finishPreparingForAgentInput];
    asb_mac_vm_set_clipboard_sync(self.vmId, NO);
}

@end
