#import "fixture-view.h"

@interface ASBFixtureDelegate : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@end
@implementation ASBFixtureDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(30, 50, 530, 655)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"AppSandbox AX Conformance";
    self.window.contentView = [[ASBFixtureView alloc] initWithFrame:NSMakeRect(0, 0, 530, 655)];
    [self.window orderBack:nil];
    if ([NSProcessInfo.processInfo.arguments containsObject:@"--guest"]) {
        [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
@end
int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication; app.activationPolicy = NSApplicationActivationPolicyRegular;
        ASBFixtureDelegate *delegate = [[ASBFixtureDelegate alloc] init]; app.delegate = delegate; [app run];
    }
    return 0;
}
