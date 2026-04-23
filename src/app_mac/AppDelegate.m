#import "AppDelegate.h"
#import "MainWindowController.h"
#import "EventLogWindow.h"
#import <WebKit/WebKit.h>

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    /* Event log window is NOT shown at launch. ui.m opens it when a VM
     * display window comes up (running state) and hides it when the last
     * one goes away. View → Event Log (⌘L) is still always available. */
    self.mainWindowController = [[MainWindowController alloc] init];
    [self.mainWindowController showWindow:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [self installMenuItems];
}

- (void)installMenuItems {
    NSMenu *mainMenu = [NSApp mainMenu];
    if (!mainMenu) return;

    /* Find or create a "View" menu. */
    NSMenuItem *viewItem = nil;
    for (NSMenuItem *it in mainMenu.itemArray) {
        if ([it.title isEqualToString:@"View"]) { viewItem = it; break; }
    }
    if (!viewItem) {
        viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:NULL keyEquivalent:@""];
        viewItem.submenu = [[NSMenu alloc] initWithTitle:@"View"];
        /* Insert before Window/Help. */
        NSUInteger insertAt = mainMenu.itemArray.count - 1;
        for (NSUInteger i = 0; i < mainMenu.itemArray.count; i++) {
            NSString *t = mainMenu.itemArray[i].title;
            if ([t isEqualToString:@"Window"] || [t isEqualToString:@"Help"]) {
                insertAt = i; break;
            }
        }
        [mainMenu insertItem:viewItem atIndex:insertAt];
    }

    NSMenuItem *logItem = [[NSMenuItem alloc]
        initWithTitle:@"Event Log"
               action:@selector(toggleEventLog:)
        keyEquivalent:@"l"];
    logItem.target = self;
    logItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [viewItem.submenu addItem:logItem];

    NSMenuItem *inspectItem = [[NSMenuItem alloc]
        initWithTitle:@"Web Inspector"
               action:@selector(openWebInspector:)
        keyEquivalent:@"i"];
    inspectItem.target = self;
    inspectItem.keyEquivalentModifierMask = NSEventModifierFlagCommand
                                          | NSEventModifierFlagOption;
    [viewItem.submenu addItem:inspectItem];
}

- (void)toggleEventLog:(id)sender {
    (void)sender;
    [[EventLogWindow shared] toggle];
}

/* Open the WKWebView's Safari Web Inspector via the private -_inspector SPI.
 * Called by the View → Web Inspector menu item (⌘⌥I). Requires that the
 * webview has inspectable = YES (set in MainWindowController). */
- (void)openWebInspector:(id)sender {
    (void)sender;
    WKWebView *wv = self.mainWindowController.webView;
    if (!wv) return;
    SEL inspectorSel = NSSelectorFromString(@"_inspector");
    if (![wv respondsToSelector:inspectorSel]) {
        NSLog(@"[inspector] _inspector SPI unavailable on this WKWebView");
        return;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    id inspector = [wv performSelector:inspectorSel];
    if (!inspector) return;
    SEL showSel = NSSelectorFromString(@"show");
    if ([inspector respondsToSelector:showSel]) {
        [inspector performSelector:showSel];
    }
#pragma clang diagnostic pop
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

@end
