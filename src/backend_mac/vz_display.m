#import "vz_display.h"
#import "vz_vm.h"
#import "asb_core_mac.h"

@interface VzDisplayWindow () <NSWindowDelegate>
@property (nonatomic, strong) VzVm *vm;
@property (nonatomic, strong) VZVirtualMachineView *vmView;
@end

@implementation VzDisplayWindow

- (instancetype)initWithVzVm:(VzVm *)vm {
    NSRect frame = NSMakeRect(0, 0, 1280, 800);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    window.title = [NSString stringWithFormat:@"%@ — Display", vm.name];
    [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _vm = vm;
    _vmView = [[VZVirtualMachineView alloc] initWithFrame:frame];
    _vmView.virtualMachine = vm.machine;
    _vmView.capturesSystemKeys = YES;
    _vmView.automaticallyReconfiguresDisplay = YES;
    _vmView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = _vmView;
    window.delegate = self;
    return self;
}

- (void)showDisplay {
    [self showWindow:nil];
    [self.window makeKeyAndOrderFront:nil];
    /* VZ doesn't surface view-attach events to the guest, so the host
     * pushes mute / clipboard-sync commands to the guest agent on
     * show / focus changes. No-ops if the agent isn't online yet. */
    asb_mac_vm_set_audio_muted(self.vm.name.UTF8String, NO);
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, YES);
}

#pragma mark - NSWindowDelegate

- (void)windowWillClose:(NSNotification *)notification {
    asb_mac_vm_set_audio_muted(self.vm.name.UTF8String, YES);
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, NO);
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    asb_mac_vm_set_audio_muted(self.vm.name.UTF8String, YES);
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, NO);
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
    asb_mac_vm_set_audio_muted(self.vm.name.UTF8String, NO);
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, YES);
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, YES);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    asb_mac_vm_set_clipboard_sync(self.vm.name.UTF8String, NO);
}

@end
