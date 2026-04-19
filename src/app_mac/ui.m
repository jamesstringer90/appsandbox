/*
 * ui.m -- JS <-> native bridge for the macOS host.
 *
 * Speaks the same action/event protocol as the Windows WebView2 bridge
 * (see src/app_win/ui.c) so a single web/app.js drives both platforms.
 */

#import "ui.h"
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "asb_core_mac.h"
#import "vz_display.h"
#import "host_info.h"
#import "EventLogWindow.h"

#include "asb_types.h"

#pragma mark - Static state

static __weak WKWebView *g_webView;

static NSMutableArray<NSString *> *g_vmNames;

static BOOL g_initialized;

#pragma mark - JSON helpers

static NSString *JSONStringFromObject(id obj) {
    if (!obj) return @"null";
    NSData *data = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    if (!data) return @"null";
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static NSString *escapeForJsCall(NSString *json) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:@[json] options:0 error:nil];
    NSString *literal = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return literal;
}

static void postToJs(NSDictionary *message) {
    WKWebView *wv = g_webView;
    if (!wv || !message) return;
    NSString *json = JSONStringFromObject(message);
    NSString *literal = escapeForJsCall(json);
    NSString *script = [NSString stringWithFormat:
        @"window.onHostMessage && window.onHostMessage(JSON.parse(%@[0]))", literal];
    dispatch_async(dispatch_get_main_queue(), ^{
        [wv evaluateJavaScript:script completionHandler:nil];
    });
}

#pragma mark - Event translation

static NSDictionary *vmToJsDict(const AsbVmMac *vm) {
    return @{
        @"name":            [NSString stringWithUTF8String:vm->name],
        @"osType":          [NSString stringWithUTF8String:vm->os_type],
        @"running":         @(vm->running || (!vm->install_complete && vm->install_progress >= 0)),
        @"shuttingDown":    @(vm->shutting_down ? YES : NO),
        @"agentOnline":     @(vm->agent_online ? YES : NO),
        @"ramMb":           @(vm->ram_mb),
        @"hddGb":           @(vm->hdd_gb),
        @"cpuCores":        @(vm->cpu_cores),
        @"gpuMode":         @(vm->gpu_mode),
        @"gpuName":         [HostInfo hostGpuName],
        @"networkMode":     @(vm->network_mode),
        @"netAdapter":      @"",
        @"isTemplate":      @NO,
        @"hypervVideoOff":  @NO,
        @"buildingVhdx":    @NO,
        @"vhdxStaging":     @NO,
        @"vhdxProgress":    @(vm->install_progress < 0 ? 0 : vm->install_progress),
        @"installComplete": @(vm->install_complete ? YES : NO),
        @"installStatus":   [NSString stringWithUTF8String:vm->install_status],
        @"sshEnabled":      @(vm->ssh_enabled ? YES : NO),
        @"sshPort":         @(vm->ssh_port),
        @"sshState":        @(vm->ssh_state),
        @"snapCurrent":     @(-1),
        @"snapCurrentBranch": @(-1),
        @"hasSnapshots":    @NO,
        @"baseBranches":    @[],
        @"snapshots":       @[],
    };
}

static NSDictionary *buildHostInfoDict(void) {
    int vmCores = 0;
    int vmRamMb = 0;
    int vmHddGb = 0;
    int count = asb_mac_vm_count();
    for (int i = 0; i < count; i++) {
        AsbVmMac *vm = asb_mac_vm_get(i);
        if (vm->running) {
            vmCores += vm->cpu_cores;
            vmRamMb += vm->ram_mb;
        }
        vmHddGb += vm->hdd_gb;
    }
    return @{
        @"hostCores": @([HostInfo hostCores]),
        @"hostRamMb": @([HostInfo hostRamMb]),
        @"vmCores":   @(vmCores),
        @"vmRamMb":   @(vmRamMb),
        @"freeGb":    @([HostInfo freeGb]),
        @"vmHddGb":   @(vmHddGb),
    };
}

static void collectVms(NSMutableArray<NSDictionary *> *outJs,
                        NSMutableArray<NSString *> *outNames) {
    int count = asb_mac_vm_count();
    for (int i = 0; i < count; i++) {
        AsbVmMac *vm = asb_mac_vm_get(i);
        [outJs addObject:vmToJsDict(vm)];
        [outNames addObject:[NSString stringWithUTF8String:vm->name]];
    }
}

static void sendVmListChanged(void) {
    NSMutableArray<NSDictionary *> *jsVms = [NSMutableArray array];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    collectVms(jsVms, names);

    g_vmNames = names;

    postToJs(@{
        @"type": @"vmListChanged",
        @"vms": jsVms,
        @"hostInfo": buildHostInfoDict(),
    });
}

static void sendFullState(void) {
    NSMutableArray<NSDictionary *> *jsVms = [NSMutableArray array];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    collectVms(jsVms, names);

    g_vmNames = names;

    postToJs(@{
        @"type": @"fullState",
        @"vms": jsVms,
        @"hostInfo": buildHostInfoDict(),
        @"adapters": @[],
        @"defaultAdapter": @0,
        @"templates": @[],
    });
}

static void syncEventLogVisibility(void) {
    int running = 0;
    int count = asb_mac_vm_count();
    for (int i = 0; i < count; i++) {
        AsbVmMac *vm = asb_mac_vm_get(i);
        if (vm && vm->running) running++;
    }
    if (running > 0) [[EventLogWindow shared] show];
    else             [[EventLogWindow shared] hide];
}

static void sendLog(NSString *message) {
    if (!message) return;
    postToJs(@{ @"type": @"log", @"message": message });
}

static void sendAlert(NSString *message) {
    if (!message) return;
    postToJs(@{ @"type": @"alert", @"message": message });
}

/* Diag events go to the Event Log window only — separate channel from
 * the user-visible main log (which lives in the WebView). Matches
 * Windows' IDD log split where protocol chatter is a different stream
 * from the main app log. */
static void sendDiag(NSString *message) {
    if (!message) return;
    [[EventLogWindow shared] appendLine:message];
}

#pragma mark - Orchestrator event callback

static void AsbEventCallback(int type, const char *vm_name,
                               int int_value, const char *str_value) {
    (void)vm_name; (void)int_value;
    NSString *str = str_value ? [NSString stringWithUTF8String:str_value] : nil;
    switch (type) {
        case CORE_VM_EVENT_LOG:
            sendLog(str);
            break;
        case CORE_VM_EVENT_ALERT:
            sendAlert(str);
            break;
        case CORE_VM_EVENT_DIAG:
            sendDiag(str);
            break;
        case CORE_VM_EVENT_STATE_CHANGED:
            sendVmListChanged();
            syncEventLogVisibility();
            break;
        case CORE_VM_EVENT_LIST_CHANGED:
            sendVmListChanged();
            syncEventLogVisibility();
            break;
        case CORE_VM_EVENT_PROGRESS:
        case CORE_VM_EVENT_INSTALL_STATUS:
        case CORE_VM_EVENT_AGENT_STATUS:
        default:
            sendVmListChanged();
            break;
    }
}

#pragma mark - Public entry points

/* Clear any leftover /tmp/appsandbox-ssh-*.command files from prior app
 * runs. They're harmless but have stale ssh ports baked in; easier to
 * nuke them than explain to a user who finds them. */
static void cleanupStaleSshScripts(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *tmpDir = NSTemporaryDirectory();
    NSArray *entries = [fm contentsOfDirectoryAtPath:tmpDir error:nil];
    for (NSString *f in entries) {
        if ([f hasPrefix:@"appsandbox-ssh-"] && [f hasSuffix:@".command"]) {
            [fm removeItemAtPath:[tmpDir stringByAppendingPathComponent:f]
                           error:nil];
        }
    }
}

void ui_set_webview(WKWebView *webView) {
    g_webView = webView;
    if (g_initialized) return;
    g_initialized = YES;

    cleanupStaleSshScripts();
    asb_mac_init();
    asb_mac_set_event_cb(AsbEventCallback);
    g_vmNames = [NSMutableArray array];
}

void ui_post_json(NSString *json) {
    if (!json) return;
    id parsed = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding]
                                                 options:0
                                                   error:nil];
    if ([parsed isKindOfClass:[NSDictionary class]]) postToJs(parsed);
}

#pragma mark - Incoming action dispatch

static NSString *vmNameAtIndex(id indexValue) {
    if (![indexValue respondsToSelector:@selector(intValue)]) return nil;
    int idx = [indexValue intValue];
    if (idx < 0 || idx >= (int)g_vmNames.count) return nil;
    return g_vmNames[idx];
}

static void handleCreateVm(NSDictionary *msg) {
    NSString *name      = msg[@"name"];
    NSString *osType    = msg[@"osType"] ?: @"macOS";
    NSString *image     = msg[@"imagePath"];
    NSString *adminUser = msg[@"adminUser"] ?: @"user";
    NSString *adminPass = msg[@"adminPass"] ?: @"test123";
    BOOL sshEnabled     = [msg[@"sshEnabled"] boolValue];
    int ramMb           = [msg[@"ramMb"] intValue];
    int hddGb           = [msg[@"hddGb"] intValue];
    int cpuCores        = [msg[@"cpuCores"] intValue];
    int gpuMode         = [msg[@"gpuMode"] intValue];
    int networkMode     = [msg[@"networkMode"] intValue];

    const char *imagePath = (image.length > 0) ? [image UTF8String] : NULL;
    int rc = asb_mac_vm_create([name UTF8String], [osType UTF8String],
                                ramMb, hddGb, cpuCores,
                                gpuMode, networkMode, imagePath,
                                [adminUser UTF8String], [adminPass UTF8String],
                                sshEnabled);
    if (rc != 0) {
        sendAlert([NSString stringWithFormat:@"Create failed (error %d)", rc]);
    }
}

static void handleStartVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    asb_mac_vm_start([n UTF8String]);
}

static void handleStopVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    asb_mac_vm_stop([n UTF8String], 1);
}

static void handleShutdownVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    asb_mac_vm_stop([n UTF8String], 0);
}

static void handleDeleteVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    asb_mac_vm_delete([n UTF8String]);
}

static void handleConnectDisplay(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    AsbVmMac *vm = asb_mac_vm_find([n UTF8String]);
    if (vm && vm->display) {
        [vm->display showDisplay];
    }
}

static void handleEditVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    NSString *field = msg[@"field"];
    id rawValue = msg[@"value"];
    NSString *value = [rawValue isKindOfClass:[NSString class]]
        ? rawValue : [NSString stringWithFormat:@"%@", rawValue];
    if (!n || !field || !value) return;
    asb_mac_vm_edit([n UTF8String], [field UTF8String], [value UTF8String]);
    sendVmListChanged();
}

static void handleBrowseImage(NSDictionary *msg) {
    (void)msg;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"ipsw"]];
        panel.message = @"Select a macOS restore image (.ipsw)";
        [panel beginWithCompletionHandler:^(NSModalResponse result) {
            NSString *path = @"";
            if (result == NSModalResponseOK && panel.URL) path = panel.URL.path;
            postToJs(@{ @"type": @"browseResult", @"path": path });
        }];
    });
}

void ui_handle_message(NSString *json) {
    if (json.length == 0) return;
    NSData *data = [json dataUsingEncoding:NSUTF8StringEncoding];
    NSError *err = nil;
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        NSLog(@"[ui] invalid message: %@", json);
        return;
    }
    NSDictionary *msg = parsed;
    NSString *action = msg[@"action"];
    if (action.length == 0) return;

    if ([action isEqualToString:@"uiReady"] || [action isEqualToString:@"getState"]) {
        sendFullState();
    } else if ([action isEqualToString:@"setMinSize"]) {
        /* The mac window manager handles minimum sizes itself. */
    } else if ([action isEqualToString:@"createVm"]) {
        handleCreateVm(msg);
    } else if ([action isEqualToString:@"startVm"]) {
        handleStartVm(msg);
    } else if ([action isEqualToString:@"stopVm"]) {
        handleStopVm(msg);
    } else if ([action isEqualToString:@"shutdownVm"]) {
        handleShutdownVm(msg);
    } else if ([action isEqualToString:@"deleteVm"]) {
        handleDeleteVm(msg);
    } else if ([action isEqualToString:@"connectIddVm"]) {
        handleConnectDisplay(msg);
    } else if ([action isEqualToString:@"editVm"]) {
        handleEditVm(msg);
    } else if ([action isEqualToString:@"browseImage"]) {
        handleBrowseImage(msg);
    } else if ([action isEqualToString:@"log"]) {
        NSString *message = msg[@"message"];
        if (message) sendLog(message);
    } else if ([action isEqualToString:@"selectVm"]) {
        /* Selection is a UI-local concept; the native side doesn't care. */
    } else if ([action isEqualToString:@"sshConnect"]) {
        NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
        if (!n) return;
        AsbVmMac *vm = asb_mac_vm_find([n UTF8String]);
        if (!vm) { sendLog(@"SSH: vm not found"); return; }
        if (vm->ssh_port <= 0) {
            sendLog([NSString stringWithFormat:
                @"SSH: proxy not bound yet (ssh_state=%d)", vm->ssh_state]);
            return;
        }

        /* Write a temp .command script and `open` it. Terminal.app is the
         * default handler for .command files, so it launches and runs the
         * script. No AppleScript, no Apple-Events TCC prompt. */
        NSString *user = [NSString stringWithUTF8String:
            vm->admin_user[0] ? vm->admin_user : "user"];
        /* - \033c full-reset clears the "Last login" banner + the
         *   auto-echoed path Terminal types before running a .command.
         * - UserKnownHostsFile=/dev/null: don't pollute ~/.ssh/known_hosts.
         * - StrictHostKeyChecking=no: don't prompt about unknown host.
         * - LogLevel=ERROR: suppress the "Permanently added …" INFO line. */
        NSString *body = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"printf '\\033c'\n"
            @"exec ssh -o StrictHostKeyChecking=no "
                    @"-o UserKnownHostsFile=/dev/null "
                    @"-o LogLevel=ERROR "
                    @"%@@127.0.0.1 -p %d\n",
            user, vm->ssh_port];
        NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"appsandbox-ssh-%@-%u.command",
             n, arc4random()]];
        NSError *wErr = nil;
        if (![body writeToFile:tmp atomically:YES
                      encoding:NSUTF8StringEncoding error:&wErr]) {
            sendLog([NSString stringWithFormat:@"SSH: temp script write failed: %@",
                     wErr.localizedDescription]);
            return;
        }
        chmod(tmp.fileSystemRepresentation, 0755);

        NSTask *t = [[NSTask alloc] init];
        t.launchPath = @"/usr/bin/open";
        t.arguments  = @[@"-a", @"Terminal", tmp];
        NSError *runErr = nil;
        if (![t launchAndReturnError:&runErr]) {
            sendLog([NSString stringWithFormat:@"SSH: open Terminal failed: %@",
                     runErr.localizedDescription]);
            return;
        }
        sendLog([NSString stringWithFormat:@"SSH: opened Terminal (127.0.0.1:%d as %@)",
                 vm->ssh_port, user]);
    } else if ([action isEqualToString:@"snapTake"] ||
               [action isEqualToString:@"snapDelete"] ||
               [action isEqualToString:@"snapDeleteBranch"] ||
               [action isEqualToString:@"snapRename"] ||
               [action isEqualToString:@"deleteTemplate"] ||
               [action isEqualToString:@"enableFeature"] ||
               [action isEqualToString:@"enableFeatureReboot"]) {
        sendLog([NSString stringWithFormat:@"%@ is not supported on macOS yet.", action]);
    } else {
        NSLog(@"[ui] unhandled action: %@", action);
    }
}
