#import "AppDelegate.h"
#import "MainWindowController.h"
#import "EventLogWindow.h"
#import "asb_core_mac.h"

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

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;

    NSMutableArray<NSString *> *inProgress = [NSMutableArray array];
    int count = asb_mac_vm_count();
    for (int i = 0; i < count; i++) {
        AsbVmMac *vm = asb_mac_vm_get(i);
        if (!vm) continue;
        if (!vm->install_complete && vm->install_progress >= 0) {
            [inProgress addObject:[NSString stringWithUTF8String:vm->name]];
        }
    }
    if (inProgress.count == 0) return NSTerminateNow;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = inProgress.count == 1
        ? @"Quit while creating a VM?"
        : @"Quit while creating VMs?";
    NSString *names = [inProgress componentsJoinedByString:@", "];
    NSString *listNoun = inProgress.count == 1 ? @"VM is" : @"VMs are";
    NSString *objectPronoun = inProgress.count == 1 ? @"it" : @"them";
    alert.informativeText = [NSString stringWithFormat:
        @"The following %@ still being created: %@.\n\n"
        @"Quitting now will cancel the download and delete %@.",
        listNoun, names, objectPronoun];
    [alert addButtonWithTitle:@"Quit and Delete"];
    [alert addButtonWithTitle:@"Cancel"];

    NSModalResponse resp = [alert runModal];
    if (resp != NSAlertFirstButtonReturn) return NSTerminateCancel;

    for (NSString *name in inProgress) {
        asb_mac_vm_delete(name.UTF8String);
    }
    return NSTerminateNow;
}

@end
