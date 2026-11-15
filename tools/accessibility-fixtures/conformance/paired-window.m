#import "fixture-view.h"
#import <objc/runtime.h>
#include <unistd.h>

@interface ASBTestPane : NSView
@property NSString *name;
@end
@implementation ASBTestPane
- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityIdentifier { return self.name; }
- (NSString *)accessibilityLabel { return self.name; }
@end

@interface ASBPairedTestPanel : NSObject
@property(weak) NSWindow *window;
@property NSTextField *status, *filter;
@property NSTask *runner;
@property NSString *lastRun;
- (instancetype)initWithWindow:(NSWindow *)window;
@end
@implementation ASBPairedTestPanel
+ (void)load {
    @autoreleasepool {
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            NSWindow *window = notification.object;
            NSString *className = NSStringFromClass(window.contentView.class);
            if (![@[@"VzAccessibilityView", @"IddDisplayView"] containsObject:className] || objc_getAssociatedObject(window, @selector(initWithWindow:))) return;
            ASBPairedTestPanel *panel = [[ASBPairedTestPanel alloc] initWithWindow:window];
            objc_setAssociatedObject(window, @selector(initWithWindow:), panel, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }];
    }
}
- (instancetype)initWithWindow:(NSWindow *)window {
    if (!(self = [super init])) return nil;
    self.window = window;
    NSView *viewer = window.contentView;
    CGFloat width = MAX(1200, MIN(1800, window.screen.visibleFrame.size.width - 60));
    CGFloat height = MAX(700, MIN(950, window.screen.visibleFrame.size.height - 80));
    [window setContentSize:NSMakeSize(width, height)];
    NSView *container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    ASBTestPane *native = [[ASBTestPane alloc] initWithFrame:NSMakeRect(0, 0, 530, height - 70)];
    native.name = @"asb-native-pane"; native.autoresizingMask = NSViewHeightSizable;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:native.bounds]; scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.documentView = [[ASBFixtureView alloc] initWithFrame:NSMakeRect(0, 0, 520, 655)]; [native addSubview:scroll];
    ASBTestPane *remote = [[ASBTestPane alloc] initWithFrame:NSMakeRect(535, 0, width - 535, height - 40)];
    remote.name = @"asb-remote-pane"; remote.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    viewer.frame = remote.bounds; viewer.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [container addSubview:native]; [container addSubview:remote];
    self.filter = [[NSTextField alloc] initWithFrame:NSMakeRect(12, height - 32, 180, 24)];
    self.filter.placeholderString = @"Case filter (default: *)"; self.filter.accessibilityIdentifier = @"asb-test-filter";
    self.filter.autoresizingMask = NSViewMinYMargin; [container addSubview:self.filter];
    NSArray *titles = @[@"Run", @"Failed", @"Stop"];
    SEL actions[] = {@selector(run:), @selector(failed:), @selector(stop:)};
    for (NSUInteger i = 0; i < titles.count; i++) {
        NSButton *button = [NSButton buttonWithTitle:titles[i] target:self action:actions[i]];
        button.frame = NSMakeRect(200 + i * 78, height - 33, 75, 26); button.autoresizingMask = NSViewMinYMargin; [container addSubview:button];
    }
    self.status = [NSTextField wrappingLabelWithString:@"Native baseline  |  Real VM viewer\nReady — tests use direct accessibility APIs."];
    self.status.frame = NSMakeRect(12, height - 68, 510, 33); self.status.font = [NSFont systemFontOfSize:11];
    self.status.autoresizingMask = NSViewMinYMargin; self.status.accessibilityIdentifier = @"asb-test-status"; [container addSubview:self.status];
    NSTextField *remoteLabel = [NSTextField labelWithString:@"Production VM viewer"];
    remoteLabel.frame = NSMakeRect(548, height - 29, 350, 22); remoteLabel.autoresizingMask = NSViewMinYMargin; [container addSubview:remoteLabel];
    window.contentView = container; [remote addSubview:viewer];
    window.minSize = NSMakeSize(1120, 730);
    __weak ASBPairedTestPanel *weak = self;
    [NSNotificationCenter.defaultCenter addObserverForName:NSWindowWillCloseNotification object:window queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
        (void)notification; [weak stop:nil];
    }];
    return self;
}
- (void)run:(id)sender { (void)sender; [self start:NO]; }
- (void)failed:(id)sender { (void)sender; [self start:YES]; }
- (void)stop:(id)sender { (void)sender; if (self.runner.running) [self.runner terminate]; }
- (void)start:(BOOL)failed {
    if (self.runner.running) return;
    NSString *directory = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"AXConformance"];
    NSString *source = [NSBundle.mainBundle objectForInfoDictionaryKey:@"ASBAXConformanceSourceDirectory"];
    if (source.length && [NSFileManager.defaultManager isExecutableFileAtPath:[source stringByAppendingPathComponent:@"build/ax-probe"]] &&
        [NSFileManager.defaultManager isExecutableFileAtPath:[source stringByAppendingPathComponent:@"build/ax-suite"]]) directory = source;
    NSString *vm = [self.window.title stringByReplacingOccurrencesOfString:@" — Display" withString:@""];
    NSString *output = [@"/tmp" stringByAppendingPathComponent:[@"asb-ax-run-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSString *runner = [directory stringByAppendingPathComponent:@"build/ax-suite"];
    NSMutableArray *arguments = [@[@"run", @"--vm", vm,
        @"--paired-pid", [NSString stringWithFormat:@"%d", NSProcessInfo.processInfo.processIdentifier], @"--window", self.window.title,
        @"--output", output, @"--cases", self.filter.stringValue.length ? self.filter.stringValue : @"*"] mutableCopy];
    if (failed && self.lastRun) [arguments addObjectsFromArray:@[@"--failed-from", self.lastRun]];
    self.lastRun = output;
    NSTask *task = [[NSTask alloc] init]; task.executableURL = [NSURL fileURLWithPath:runner]; task.arguments = arguments;
    NSString *user = NSProcessInfo.processInfo.environment[@"SUDO_USER"];
    if (geteuid() == 0 && user.length) {
        task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/sudo"];
        task.arguments = [@[@"-H", @"-u", user, @"--", @"/usr/bin/env", @"-u", @"SUDO_USER", @"-u", @"SUDO_UID", @"-u", @"SUDO_GID", runner] arrayByAddingObjectsFromArray:arguments];
    }
    NSPipe *pipe = NSPipe.pipe; task.standardOutput = pipe; task.standardError = pipe;
    __weak ASBPairedTestPanel *weak = self;
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        if (!data.length) { handle.readabilityHandler = nil; return; }
        NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        dispatch_async(dispatch_get_main_queue(), ^{ if (text.length) weak.status.stringValue = text; });
    };
    NSError *error = nil;
    if ([task launchAndReturnError:&error]) { self.runner = task; self.status.stringValue = @"Starting paired tests…"; }
    else self.status.stringValue = error.localizedDescription;
}
@end
