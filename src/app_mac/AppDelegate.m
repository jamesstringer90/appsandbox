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
    if (!mainMenu) {
        /* The app is built programmatically (no nib / no setMainMenu:), so
         * [NSApp mainMenu] is nil here. Build a minimal menu bar with an
         * application menu (Quit) to host the View → Event Log item below. */
        mainMenu = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [mainMenu addItem:appItem];
        NSMenu *appSubmenu = [[NSMenu alloc] init];
        appItem.submenu = appSubmenu;
        NSString *appName = [[NSProcessInfo processInfo] processName];
        [appSubmenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                              action:@selector(terminate:)
                       keyEquivalent:@"q"];
        [NSApp setMainMenu:mainMenu];
    }

    /* Find or create a "View" menu. */
    NSMenuItem *viewItem = nil;
    for (NSMenuItem *it in mainMenu.itemArray) {
        if ([it.title isEqualToString:@"View"]) { viewItem = it; break; }
    }
    if (!viewItem) {
        viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:NULL keyEquivalent:@""];
        viewItem.submenu = [[NSMenu alloc] initWithTitle:@"View"];
        /* Insert before Window/Help, else append at the end. */
        NSUInteger insertAt = mainMenu.itemArray.count;
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

    NSMenuItem *borderlessItem = [[NSMenuItem alloc]
        initWithTitle:@"Toggle Borderless Full Screen"
               action:@selector(toggleBorderlessFullscreen:)
        keyEquivalent:[NSString stringWithFormat:@"%C", NSF11FunctionKey]];
    borderlessItem.target = self;
    [viewItem.submenu addItem:borderlessItem];
}

- (void)toggleBorderlessFullscreen:(id)sender {
    id controller = NSApp.keyWindow.windowController;
    if (controller && [controller respondsToSelector:_cmd]) {
        [controller toggleBorderlessFullscreen:sender];
    } else {
        [self.mainWindowController toggleBorderlessFullscreen:sender];
    }
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
        /* Warn only while the host disk-build is in flight (iso-patch owns the disk;
           quitting orphans the child + a half-built disk). Once disk_built, a Windows
           VM may still be "installing" its first boot, but quitting just stops QEMU
           normally — no warning needed. Matches vm_installing in headless.m. */
        if (!vm->disk_built && vm->install_progress >= 0) {
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

/* Fires after applicationShouldTerminate: returns NSTerminateNow, on the main
 * thread, just before exit. A running VZ guest dies on its own when its
 * in-process VZVirtualMachine is released at process teardown, but a Windows
 * guest's QEMU is an external child that would otherwise orphan (and keep the
 * instance lock). asb_mac_cleanup stops QEMU + the channel helpers so every VM
 * dies with the app, matching the macOS-guest behavior. */
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    asb_mac_cleanup();
}

@end
