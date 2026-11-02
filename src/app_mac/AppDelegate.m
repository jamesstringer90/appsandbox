#import "AppDelegate.h"
#import "MainWindowController.h"
#import "asb_core_mac.h"
#import "ui.h"
#include "asb_types.h"
#include "asb_events.h"

typedef NS_ENUM(NSInteger, VmMenuAction) {
    VmMenuConnect = 1,
    VmMenuShutdown,
    VmMenuForceStop,
};

@interface AppDelegate () <NSMenuDelegate, NSMenuItemValidation>
@property (nonatomic, strong) NSStatusItem *statusItem;
@property (nonatomic) BOOL statusMenuOpen;
@property (nonatomic, strong) id statusMouseMonitor;
@property (nonatomic, strong) id statusActivationObserver;
@property (nonatomic) unsigned statusSubscription;
- (void)refreshStatusMenu;
@end

static __weak AppDelegate *statusMenuDelegate;

static void statusCoreEvent(const CoreVmEvent *event, void *context) {
    (void)context;
    switch (event->type) {
        case CORE_VM_EVENT_LIST_CHANGED:
        case CORE_VM_EVENT_STATE_CHANGED:
        case CORE_VM_EVENT_AGENT_STATUS:
        case CORE_VM_EVENT_PROGRESS:
        case CORE_VM_EVENT_INSTALL_STATUS: break;
        default: return;
    }
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        AppDelegate *delegate = statusMenuDelegate;
        if (delegate.statusMenuOpen) [delegate refreshStatusMenu];
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

static NSImage *menuBarLogo(void) {
    NSImage *icon = NSApp.applicationIconImage;
    NSRect proposed = NSMakeRect(0, 0, 1024, 1024);
    CGImageRef source = [icon CGImageForProposedRect:&proposed context:nil hints:nil];
    NSBitmapImageRep *bitmap = source ? [[NSBitmapImageRep alloc] initWithCGImage:source] : nil;
    NSInteger width = bitmap.pixelsWide, height = bitmap.pixelsHigh;
    NSInteger left = width, top = height, right = -1, bottom = -1;
    if (bitmap.hasAlpha && bitmap.samplesPerPixel <= 16) {
        NSInteger alpha = (bitmap.bitmapFormat & NSBitmapFormatAlphaFirst) ? 0 : bitmap.samplesPerPixel - 1;
        NSUInteger pixel[16];
        for (NSInteger y = 0; y < height; ++y) {
            for (NSInteger x = 0; x < width; ++x) {
                [bitmap getPixel:pixel atX:x y:y];
                if (!pixel[alpha]) continue;
                left = MIN(left, x); right = MAX(right, x);
                top = MIN(top, y); bottom = MAX(bottom, y);
            }
        }
    }
    NSImage *image;
    if (right >= left && bottom >= top) {
        CGRect bounds = CGRectMake(left, top, right - left + 1, bottom - top + 1);
        CGImageRef cropped = CGImageCreateWithImageInRect(bitmap.CGImage, bounds);
        image = [[NSImage alloc] initWithCGImage:cropped
                                         size:NSMakeSize(18.0 * bounds.size.width / bounds.size.height, 18)];
        CGImageRelease(cropped);
    } else {
        image = [icon copy];
        image.size = NSMakeSize(18, 18);
    }
    image.template = NO;
    return image;
}

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    if (!self.headlessQuitHandler) {
        self.mainWindowController = [[MainWindowController alloc] init];
        [self.mainWindowController showWindow:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [self installMenuItems];
    }
    @try {
        [self installStatusItem];
    } @catch (NSException *exception) {
        NSLog(@"AppSandbox menu bar unavailable: %@", exception.reason);
    }
}

- (void)installStatusItem {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    if (!self.statusItem) return;
    self.statusItem.button.image = menuBarLogo();
    self.statusItem.button.toolTip = @"AppSandbox";
    [self.statusItem.button setAccessibilityLabel:@"AppSandbox"];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"AppSandbox"];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    NSMenuItem *show = [menu addItemWithTitle:self.headlessQuitHandler ? @"Headless API" : @"Show AppSandbox"
                                     action:self.headlessQuitHandler ? NULL : @selector(showMainWindow:)
                              keyEquivalent:@""];
    show.target = self;
    show.enabled = !self.headlessQuitHandler;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *empty = [menu addItemWithTitle:@"No VMs running" action:nil keyEquivalent:@""];
    empty.enabled = NO;
    empty.tag = -1;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [menu addItemWithTitle:@"Quit AppSandbox" action:@selector(terminate:) keyEquivalent:@""];
    quit.target = NSApp;
    self.statusItem.menu = menu;
    statusMenuDelegate = self;
    self.statusSubscription = asb_events_subscribe(statusCoreEvent, NULL);
}

- (void)menuWillOpen:(NSMenu *)menu {
    if (menu != self.statusItem.menu || self.statusMenuOpen) return;
    self.statusMenuOpen = YES;
    __weak AppDelegate *weakSelf = self;
    self.statusMouseMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:
        NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown
        handler:^(NSEvent *event) {
            (void)event;
            [weakSelf.statusItem.menu cancelTrackingWithoutAnimation];
        }];
    self.statusActivationObserver = [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:nil
        usingBlock:^(NSNotification *notification) {
            NSRunningApplication *app = notification.userInfo[NSWorkspaceApplicationKey];
            if (app.processIdentifier != NSProcessInfo.processInfo.processIdentifier)
                [weakSelf.statusItem.menu cancelTrackingWithoutAnimation];
        }];
}

- (void)stopStatusMenuTracking {
    self.statusMenuOpen = NO;
    if (self.statusMouseMonitor) [NSEvent removeMonitor:self.statusMouseMonitor];
    self.statusMouseMonitor = nil;
    if (self.statusActivationObserver)
        [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self.statusActivationObserver];
    self.statusActivationObserver = nil;
}

- (void)menuDidClose:(NSMenu *)menu {
    if (menu == self.statusItem.menu) [self stopStatusMenuTracking];
}

- (void)applicationDidResignActive:(NSNotification *)notification {
    (void)notification;
    if (self.statusMenuOpen) [self.statusItem.menu cancelTrackingWithoutAnimation];
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    if (menu != self.statusItem.menu) return;
    [self refreshStatusMenu];
}

- (void)refreshStatusMenu {
    NSMenu *menu = self.statusItem.menu;
    if (!menu) return;
    NSMutableDictionary<NSNumber *, NSMenuItem *> *entries = [NSMutableDictionary dictionary];
    for (NSMenuItem *item in menu.itemArray)
        if (item.representedObject) entries[item.representedObject] = item;

    int runningCount = 0;
    for (int i = 0; i < asb_mac_vm_count(); ++i) {
        AsbVmMac *vm = asb_mac_vm_get(i);
        if (!vm || (!vm->running && !vm->starting)) continue;
        runningCount++;
        NSString *state = vm->starting ? @"Starting" : vm->shutting_down ? @"Shutting down"
                         : !vm->install_complete ? @"Installing" : !vm->agent_online ? @"Booting" : @"Running";
        NSNumber *vmId = @(vm->unique_id);
        NSMenuItem *entry = entries[vmId];
        if (!entry) {
            entry = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
            entry.representedObject = vmId;
            NSMenu *actions = [[NSMenu alloc] initWithTitle:@(vm->name)];
            actions.autoenablesItems = NO;
            entry.submenu = actions;
            NSArray<NSString *> *titles = @[@"Connect", @"Shutdown", @"Force Stop"];
            for (NSInteger action = VmMenuConnect; action <= VmMenuForceStop; ++action) {
                NSMenuItem *item = [actions addItemWithTitle:titles[action - 1]
                                                    action:@selector(performVmMenuAction:)
                                             keyEquivalent:@""];
                item.target = self;
                item.tag = action;
                item.representedObject = vmId;
            }
        }
        [entries removeObjectForKey:vmId];
        NSInteger position = runningCount + 1;
        if ([menu indexOfItem:entry] != position) {
            if (entry.menu == menu) [menu removeItem:entry];
            [menu insertItem:entry atIndex:position];
        }
        entry.title = [NSString stringWithFormat:@"%s — %@", vm->name, state];
        entry.submenu.title = @(vm->name);
        for (NSMenuItem *item in entry.submenu.itemArray)
            item.enabled = [self validateMenuItem:item];
    }
    for (NSMenuItem *removed in entries.allValues) [menu removeItem:removed];
    [menu itemWithTag:-1].hidden = runningCount != 0;
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action != @selector(performVmMenuAction:)) return YES;
    AsbVmMac *vm = asb_mac_vm_find_by_id([item.representedObject unsignedLongLongValue]);
    if (!vm) return NO;
    switch (item.tag) {
        case VmMenuConnect:
            return vm->running && !vm->shutting_down && vm->agent_online &&
                   (strcasecmp(vm->os_type, "Windows") != 0 || vm->idd_ready);
        case VmMenuShutdown: return vm->running && !vm->shutting_down;
        case VmMenuForceStop: return vm->running || vm->starting;
        default: return NO;
    }
}

- (void)performVmMenuAction:(NSMenuItem *)item {
    if (![self validateMenuItem:item]) return;
    AsbVmId vm_id = [item.representedObject unsignedLongLongValue];
    int rc = BACKEND_OK;
    switch (item.tag) {
        case VmMenuConnect: rc = asb_mac_open_display(vm_id); break;
        case VmMenuShutdown: rc = asb_mac_vm_stop(vm_id, 0); break;
        case VmMenuForceStop: rc = asb_mac_vm_stop(vm_id, 1); break;
    }
    if (rc != BACKEND_OK && rc != BACKEND_ERR_NOT_FOUND && rc != BACKEND_ERR_NOT_RUNNING) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"The VM action could not complete.";
        alert.informativeText = self.headlessQuitHandler
            ? @"Check the local API or headless.log for the VM's current state."
            : @"Open AppSandbox to check the VM's current state and app log.";
        [alert runModal];
    }
}

- (void)showMainWindow:(id)sender {
    (void)sender;
    NSWindow *window = self.mainWindowController.window;
    if (window.isMiniaturized) [window deminiaturize:nil];
    [self.mainWindowController showWindow:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible {
    (void)sender; (void)visible;
    if (self.headlessQuitHandler) return NO;
    [self showMainWindow:nil];
    return YES;
}

- (void)installMenuItems {
    NSMenu *mainMenu = [NSApp mainMenu];
    if (!mainMenu) mainMenu = [[NSMenu alloc] init];
    NSMenuItem *appItem = mainMenu.numberOfItems ? [mainMenu itemAtIndex:0] : nil;
    if (!appItem) {
        appItem = [[NSMenuItem alloc] initWithTitle:@"AppSandbox" action:nil keyEquivalent:@""];
        [mainMenu addItem:appItem];
    }
    NSMenu *appMenu = appItem.submenu ?: [[NSMenu alloc] initWithTitle:@"AppSandbox"];
    appItem.submenu = appMenu;
    NSMenuItem *quit = nil;
    for (NSMenuItem *item in appMenu.itemArray)
        if (item.action == @selector(terminate:)) { quit = item; break; }
    if (!quit) quit = [appMenu addItemWithTitle:@"Quit AppSandbox"
                                        action:@selector(terminate:) keyEquivalent:@"q"];
    quit.target = NSApp;
    quit.keyEquivalent = @"q";
    quit.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [NSApp setMainMenu:mainMenu];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return NO;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;

    NSMutableArray<NSString *> *inProgress = [NSMutableArray array];
    NSMutableArray<NSString *> *provisioning = [NSMutableArray array];
    int count = asb_mac_vm_count();
    for (int i = 0; i < count; i++) {
        AsbVmMac *vm = asb_mac_vm_get(i);
        if (!vm) continue;
        if (vm->disk_built && vm->mac_provisioning_pending)
            [provisioning addObject:[NSString stringWithUTF8String:vm->name]];
        if (!vm->disk_built && vm->install_progress >= 0) {
            [inProgress addObject:[NSString stringWithUTF8String:vm->name]];
        }
    }
    if (provisioning.count) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"Quit during macOS setup?";
        alert.informativeText = [NSString stringWithFormat:
            @"Account setup has not finished for %@. Keep AppSandbox open until setup completes. "
             "If you quit now, these unfinished VMs will need to be recreated.",
            [provisioning componentsJoinedByString:@", "]];
        [alert addButtonWithTitle:@"Keep AppSandbox Open"];
        [alert addButtonWithTitle:@"Quit"];
        if ([alert runModal] != NSAlertSecondButtonReturn) return NSTerminateCancel;
    }
    if (inProgress.count == 0) {
        if (self.headlessQuitHandler) self.headlessQuitHandler();
        return NSTerminateNow;
    }

    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = inProgress.count == 1
        ? @"Quit while creating a VM?"
        : @"Quit while creating VMs?";
    NSString *names = [inProgress componentsJoinedByString:@", "];
    NSString *listNoun = inProgress.count == 1 ? @"VM is" : @"VMs are";
    alert.informativeText = [NSString stringWithFormat:
        @"The following %@ still being created: %@.\n\n"
        @"Quitting will interrupt creation. You can delete unfinished VMs after reopening AppSandbox.",
        listNoun, names];
    [alert addButtonWithTitle:@"Quit"];
    [alert addButtonWithTitle:@"Cancel"];

    NSModalResponse resp = [alert runModal];
    if (resp != NSAlertFirstButtonReturn) return NSTerminateCancel;

    if (self.headlessQuitHandler) self.headlessQuitHandler();
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
    asb_events_unsubscribe(self.statusSubscription);
    statusMenuDelegate = nil;
    [self stopStatusMenuTracking];
    [[NSStatusBar systemStatusBar] removeStatusItem:self.statusItem];
    self.statusItem = nil;
    ui_shutdown();
    asb_mac_cleanup();
}

@end
