#import "AppDelegate.h"
#import "MainWindowController.h"
#import "EventLogWindow.h"

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
}

- (void)toggleEventLog:(id)sender {
    (void)sender;
    [[EventLogWindow shared] toggle];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

@end
