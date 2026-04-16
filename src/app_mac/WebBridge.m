/*
 * WebBridge.m -- JS <-> native bridge for the macOS host.
 *
 * Speaks the same action/event protocol as the Windows WebView2 bridge
 * (see src/ui.c) so a single web/app.js drives both platforms. Actions
 * arrive from JS as a JSON string on the "host" WKScriptMessageHandler;
 * outgoing events are serialized and handed to window.onHostMessage via
 * evaluateJavaScript:.
 */

#import "WebBridge.h"
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "host_info.h"

#include "backend.h"
#include "vm_types.h"

#pragma mark - Static state

static __weak WKWebView *g_webView;

/* Cached name list in the same order as the last list_vms() call, so
 * vmIndex values from the UI can be resolved to VM names. */
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
    /* Embed the outgoing payload as a JSON string literal, then call
     * JSON.parse at the receiving end. This is the only escape-safe way
     * to pass arbitrary text through evaluateJavaScript. */
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

static NSDictionary *vmInfoToJsDict(const CoreVmInfo *info) {
    /* Emit every field the JS side expects from a Windows VM row, with
     * sensible defaults for the Windows-only concepts. Missing fields make
     * the UI throw, so this list must stay in sync with build_vm_json()
     * in src/ui.c and the fields referenced by web/app.js. */
    return @{
        @"name":            [NSString stringWithUTF8String:info->name],
        @"osType":          [NSString stringWithUTF8String:info->os_type],
        @"running":         @(info->running ? YES : NO),
        @"shuttingDown":    @(info->shutting_down ? YES : NO),
        @"agentOnline":     @NO,
        @"ramMb":           @(info->ram_mb),
        @"hddGb":           @(info->hdd_gb),
        @"cpuCores":        @(info->cpu_cores),
        @"gpuMode":         @0,
        @"gpuName":         @"",
        @"networkMode":     @1,  /* NAT */
        @"netAdapter":      @"",
        @"isTemplate":      @NO,
        @"hypervVideoOff":  @NO,
        @"buildingVhdx":    @NO,
        @"vhdxStaging":     @NO,
        @"vhdxProgress":    @(info->install_progress < 0 ? 0 : info->install_progress),
        @"installComplete": @(info->install_complete ? YES : NO),
        @"installStatus":   [NSString stringWithUTF8String:info->install_status],
        @"sshEnabled":      @NO,
        @"sshPort":         @0,
        @"sshState":        @0,
        @"snapCurrent":     @(-1),
        @"snapCurrentBranch": @(-1),
        @"hasSnapshots":    @NO,
        @"baseBranches":    @[],
        @"snapshots":       @[],
    };
}

static NSDictionary *buildHostInfoDict(const CoreVmInfo *vms, int count) {
    int vmCores = 0;
    int vmRamMb = 0;
    int vmHddGb = 0;
    for (int i = 0; i < count; i++) {
        if (vms[i].running) {
            vmCores += vms[i].cpu_cores;
            vmRamMb += vms[i].ram_mb;
        }
        vmHddGb += vms[i].hdd_gb;
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

static NSArray *collectVms(NSMutableArray<NSDictionary *> *outJs,
                           NSMutableArray<NSString *> *outNames,
                           CoreVmInfo *outInfos,
                           int *outCount) {
    const BackendVtbl *b = backend_get();
    int count = 0;
    if (b->list_vms) {
        b->list_vms(outInfos, 64, &count);
    }
    for (int i = 0; i < count; i++) {
        [outJs addObject:vmInfoToJsDict(&outInfos[i])];
        [outNames addObject:[NSString stringWithUTF8String:outInfos[i].name]];
    }
    if (outCount) *outCount = count;
    return outJs;
}

static void sendVmListChanged(void) {
    NSMutableArray<NSDictionary *> *jsVms = [NSMutableArray array];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    CoreVmInfo infos[64];
    int count = 0;
    collectVms(jsVms, names, infos, &count);

    g_vmNames = names;

    NSDictionary *msg = @{
        @"type": @"vmListChanged",
        @"vms": jsVms,
        @"hostInfo": buildHostInfoDict(infos, count),
    };
    postToJs(msg);
}

static void sendFullState(void) {
    NSMutableArray<NSDictionary *> *jsVms = [NSMutableArray array];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    CoreVmInfo infos[64];
    int count = 0;
    collectVms(jsVms, names, infos, &count);

    g_vmNames = names;

    NSDictionary *msg = @{
        @"type": @"fullState",
        @"vms": jsVms,
        @"hostInfo": buildHostInfoDict(infos, count),
        @"adapters": @[],       /* bridged networking is not exposed yet on mac */
        @"defaultAdapter": @0,
        @"templates": @[],      /* templates are not yet implemented on mac */
    };
    postToJs(msg);
}

static void sendLog(NSString *message) {
    if (!message) return;
    postToJs(@{ @"type": @"log", @"message": message });
}

static void sendAlert(NSString *message) {
    if (!message) return;
    postToJs(@{ @"type": @"alert", @"message": message });
}

#pragma mark - Backend event callback

static void BackendEventCallback(const CoreVmEvent *event, void *user_data) {
    (void)user_data;
    if (!event) return;
    NSString *str = event->str_value ? [NSString stringWithUTF8String:event->str_value] : nil;
    switch (event->type) {
        case CORE_VM_EVENT_LOG:
            sendLog(str);
            break;
        case CORE_VM_EVENT_ALERT:
            sendAlert(str);
            break;
        case CORE_VM_EVENT_STATE_CHANGED:
        case CORE_VM_EVENT_PROGRESS:
        case CORE_VM_EVENT_INSTALL_STATUS:
        case CORE_VM_EVENT_LIST_CHANGED:
        default:
            sendVmListChanged();
            break;
    }
}

#pragma mark - Public entry points

void WebBridgeSetWebView(WKWebView *webView) {
    g_webView = webView;
    if (g_initialized) return;
    g_initialized = YES;

    const BackendVtbl *b = backend_get();
    if (b->init) b->init();
    if (b->set_event_cb) b->set_event_cb(BackendEventCallback, NULL);
    g_vmNames = [NSMutableArray array];
}

void WebBridgePostJson(NSString *json) {
    /* Legacy entry — the rest of the bridge posts structured dicts. Kept
     * for backwards compat in case anything else calls it. */
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

static const char *utf8OrNull(NSString *s) {
    return s.length > 0 ? [s UTF8String] : NULL;
}

static void handleCreateVm(NSDictionary *msg) {
    /* app.js sends the full sandbox form as top-level fields on the action. */
    NSString *name      = msg[@"name"];
    NSString *osType    = msg[@"osType"] ?: @"macOS";
    NSString *image     = msg[@"imagePath"];
    NSString *tmpl      = msg[@"templateName"];
    NSString *user      = msg[@"adminUser"];
    NSString *password  = msg[@"adminPass"];

    CoreVmConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name          = utf8OrNull(name);
    cfg.os_type       = utf8OrNull(osType);
    cfg.image_path    = utf8OrNull(image);
    cfg.template_name = utf8OrNull(tmpl);
    cfg.ram_mb        = [msg[@"ramMb"] intValue];
    cfg.hdd_gb        = [msg[@"hddGb"] intValue];
    cfg.cpu_cores     = [msg[@"cpuCores"] intValue];
    cfg.gpu_mode      = [msg[@"gpuMode"] intValue];
    cfg.network_mode  = [msg[@"networkMode"] intValue];
    cfg.net_adapter   = utf8OrNull(msg[@"netAdapter"]);
    cfg.username      = utf8OrNull(user);
    cfg.password      = utf8OrNull(password);
    cfg.is_template   = [msg[@"isTemplate"] intValue];

    char err[256] = { 0 };
    const BackendVtbl *b = backend_get();
    int rc = b->create_vm ? b->create_vm(&cfg, err, sizeof(err)) : BACKEND_ERR_NOT_IMPLEMENTED;
    if (rc != BACKEND_OK) {
        sendAlert([NSString stringWithFormat:@"Create failed: %s",
                    err[0] ? err : "unknown error"]);
    }
}

static void handleStartVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    backend_get()->start_vm([n UTF8String]);
}

static void handleStopVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    backend_get()->stop_vm([n UTF8String], 1 /* force */);
}

static void handleShutdownVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    backend_get()->stop_vm([n UTF8String], 0 /* graceful */);
}

static void handleDeleteVm(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    backend_get()->delete_vm([n UTF8String]);
}

static void handleConnectIdd(NSDictionary *msg) {
    NSString *n = vmNameAtIndex(msg[@"vmIndex"]);
    if (!n) return;
    backend_get()->open_display([n UTF8String], NULL);
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

void WebBridgeHandleMessage(NSString *json) {
    if (json.length == 0) return;
    NSData *data = [json dataUsingEncoding:NSUTF8StringEncoding];
    NSError *err = nil;
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        NSLog(@"[WebBridge] invalid message: %@", json);
        return;
    }
    NSDictionary *msg = parsed;
    NSString *action = msg[@"action"];
    if (action.length == 0) return;

    if ([action isEqualToString:@"uiReady"] || [action isEqualToString:@"getState"]) {
        sendFullState();
    } else if ([action isEqualToString:@"setMinSize"]) {
        /* The mac window manager handles minimum sizes itself. No-op. */
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
        handleConnectIdd(msg);
    } else if ([action isEqualToString:@"browseImage"]) {
        handleBrowseImage(msg);
    } else if ([action isEqualToString:@"log"]) {
        NSString *message = msg[@"message"];
        if (message) sendLog(message);
    } else if ([action isEqualToString:@"selectVm"]) {
        /* Selection is a UI-local concept; the native side doesn't care. */
    } else if ([action isEqualToString:@"editVm"] ||
               [action isEqualToString:@"snapTake"] ||
               [action isEqualToString:@"snapDelete"] ||
               [action isEqualToString:@"snapDeleteBranch"] ||
               [action isEqualToString:@"snapRename"] ||
               [action isEqualToString:@"sshConnect"] ||
               [action isEqualToString:@"deleteTemplate"] ||
               [action isEqualToString:@"enableFeature"] ||
               [action isEqualToString:@"enableFeatureReboot"]) {
        sendLog([NSString stringWithFormat:@"%@ is not supported on macOS yet.", action]);
    } else {
        NSLog(@"[WebBridge] unhandled action: %@", action);
    }
}
