#import "vz_display.h"
#import "vz_vm.h"

@interface VzDisplayWindow ()
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
    return self;
}

- (void)showDisplay {
    [self showWindow:nil];
    [self.window makeKeyAndOrderFront:nil];
}

@end
