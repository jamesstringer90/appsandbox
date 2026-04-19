/*
 * asb_core_mac.m -- macOS orchestrator implementation.
 *
 * Mirrors asb_core.c on Windows: owns the in-memory VM array (g_vms[]),
 * persists to ~/Library/Application Support/AppSandbox/vms.cfg in the
 * same INI format as Windows, and drives VM lifecycle through the VZ
 * helper modules (vz_vm, vz_install, vz_display, vz_disk, vz_network).
 */

#import "asb_core_mac.h"
#import "vm_dir.h"
#import "vz_vm.h"
#import "vz_display.h"
#import "vz_network.h"
#import "vm_agent_mac.h"
#import "vm_ssh_proxy_mac.h"
#import "vm_clipboard_mac.h"
#import "iso_patch_mac.h"
#import "host_info.h"

#include "asb_types.h"

#include <stdio.h>
#include <string.h>

/* ---- Global state ---- */

static AsbVmMac g_vms[ASB_MAX_VMS];
static int g_vm_count = 0;
static char g_last_ipsw_path[1024] = {0};
static AsbMacEventCallback g_event_cb = NULL;

/* Strong references to ObjC objects whose lifetime is tied to g_vms[].
 * The struct stores __unsafe_unretained pointers; these arrays keep them alive. */
static id g_vz_refs[ASB_MAX_VMS];
static id g_display_refs[ASB_MAX_VMS];
static id g_agent_refs[ASB_MAX_VMS];
static id g_ssh_proxy_refs[ASB_MAX_VMS];
static id g_clipboard_refs[ASB_MAX_VMS];

/* ---- Helpers ---- */

static void run_on_main(dispatch_block_t block) {
    if ([NSThread isMainThread]) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

static void post_event(int type, const char *vm_name, int int_value, const char *str_value) {
    if (!g_event_cb) return;
    g_event_cb(type, vm_name, int_value, str_value);
}

static void post_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    post_event(CORE_VM_EVENT_LOG, NULL, 0, buf);
}

/* Technical / protocol events destined for the Event Log window only.
 * Not emitted to the WebView main log (matches Windows' IDD-log split). */
static void post_diag(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    post_event(CORE_VM_EVENT_DIAG, NULL, 0, buf);
}

static void post_alert(const char *vm_name, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    post_event(CORE_VM_EVENT_ALERT, vm_name, 0, buf);
}

static void post_list_changed(void) {
    post_event(CORE_VM_EVENT_LIST_CHANGED, NULL, 0, NULL);
}

static int vm_index_of(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_vm_count; i++) {
        if (strcmp(g_vms[i].name, name) == 0) return i;
    }
    return -1;
}

/* ---- Persistence ---- */

static NSURL *config_file_url(void) {
    NSURL *root = [VmDir vmsRootDirectory];
    if (!root) return nil;
    return [[root URLByDeletingLastPathComponent] URLByAppendingPathComponent:@"vms.cfg"];
}

static void save_vm_list(void) {
    NSURL *url = config_file_url();
    if (!url) return;
    FILE *f = fopen(url.fileSystemRepresentation, "w");
    if (!f) return;

    if (g_last_ipsw_path[0]) {
        fprintf(f, "[Settings]\n");
        fprintf(f, "LastIpswPath=%s\n", g_last_ipsw_path);
        fprintf(f, "\n");
    }

    for (int i = 0; i < g_vm_count; i++) {
        fprintf(f, "[VM]\n");
        fprintf(f, "Name=%s\n", g_vms[i].name);
        fprintf(f, "OsType=%s\n", g_vms[i].os_type);
        fprintf(f, "RamMB=%d\n", g_vms[i].ram_mb);
        fprintf(f, "HddGB=%d\n", g_vms[i].hdd_gb);
        fprintf(f, "CpuCores=%d\n", g_vms[i].cpu_cores);
        fprintf(f, "GpuMode=%d\n", g_vms[i].gpu_mode);
        fprintf(f, "NetworkMode=%d\n", g_vms[i].network_mode);
        if (g_vms[i].admin_user[0])
            fprintf(f, "AdminUser=%s\n", g_vms[i].admin_user);
        /* admin_pass intentionally NOT persisted — matches Windows. */
        if (g_vms[i].ssh_enabled)
            fprintf(f, "SshEnabled=1\n");
        if (g_vms[i].ssh_port > 0)
            fprintf(f, "SshPort=%d\n", g_vms[i].ssh_port);
        if (g_vms[i].install_complete)
            fprintf(f, "InstallComplete=1\n");
        fprintf(f, "\n");
    }

    fclose(f);
}

static void load_vm_list(void) {
    NSURL *url = config_file_url();
    if (!url) return;
    FILE *f = fopen(url.fileSystemRepresentation, "r");
    if (!f) return;

    char line[1024];
    AsbVmMac *vm = NULL;
    BOOL in_settings = NO;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strcmp(line, "[Settings]") == 0) {
            in_settings = YES;
            vm = NULL;
            continue;
        }

        if (strcmp(line, "[VM]") == 0) {
            in_settings = NO;
            if (g_vm_count >= ASB_MAX_VMS) break;
            vm = &g_vms[g_vm_count];
            memset(vm, 0, sizeof(*vm));
            vm->install_progress = -1;
            g_vm_count++;
            continue;
        }

        if (in_settings) {
            if (strncmp(line, "LastIpswPath=", 13) == 0)
                strlcpy(g_last_ipsw_path, line + 13, sizeof(g_last_ipsw_path));
            continue;
        }

        if (!vm) continue;

        if (strncmp(line, "Name=", 5) == 0)
            strlcpy(vm->name, line + 5, sizeof(vm->name));
        else if (strncmp(line, "OsType=", 7) == 0)
            strlcpy(vm->os_type, line + 7, sizeof(vm->os_type));
        else if (strncmp(line, "RamMB=", 6) == 0)
            vm->ram_mb = atoi(line + 6);
        else if (strncmp(line, "HddGB=", 6) == 0)
            vm->hdd_gb = atoi(line + 6);
        else if (strncmp(line, "CpuCores=", 9) == 0)
            vm->cpu_cores = atoi(line + 9);
        else if (strncmp(line, "GpuMode=", 8) == 0)
            vm->gpu_mode = atoi(line + 8);
        else if (strncmp(line, "NetworkMode=", 12) == 0)
            vm->network_mode = atoi(line + 12);
        else if (strncmp(line, "AdminUser=", 10) == 0)
            strlcpy(vm->admin_user, line + 10, sizeof(vm->admin_user));
        else if (strncmp(line, "SshEnabled=", 11) == 0)
            vm->ssh_enabled = (atoi(line + 11) != 0);
        else if (strncmp(line, "SshPort=", 8) == 0)
            vm->ssh_port = atoi(line + 8);
        else if (strncmp(line, "InstallComplete=", 16) == 0)
            vm->install_complete = (atoi(line + 16) != 0);
    }

    fclose(f);
}

/* ---- Public: init/cleanup ---- */

void asb_mac_init(void) {
    memset(g_vms, 0, sizeof(g_vms));
    memset(g_vz_refs, 0, sizeof(g_vz_refs));
    memset(g_display_refs, 0, sizeof(g_display_refs));
    memset(g_agent_refs, 0, sizeof(g_agent_refs));
    memset(g_ssh_proxy_refs, 0, sizeof(g_ssh_proxy_refs));
    memset(g_clipboard_refs, 0, sizeof(g_clipboard_refs));
    g_vm_count = 0;
    load_vm_list();
}

void asb_mac_cleanup(void) {
    for (int i = 0; i < g_vm_count; i++) {
        if (g_clipboard_refs[i]) [(VmClipboardMac *)g_clipboard_refs[i] stop];
        g_clipboard_refs[i] = nil;
        if (g_ssh_proxy_refs[i]) [(VmSshProxyMac *)g_ssh_proxy_refs[i] stop];
        g_ssh_proxy_refs[i] = nil;
        if (g_agent_refs[i]) [(VmAgentMac *)g_agent_refs[i] stop];
        g_agent_refs[i] = nil;
        g_vz_refs[i] = nil;
        g_display_refs[i] = nil;
    }
    g_vm_count = 0;
    g_event_cb = NULL;
    [IsoPatchMac releaseAuthorization];
}

/* ---- Public: array access ---- */

int asb_mac_vm_count(void) {
    return g_vm_count;
}

AsbVmMac *asb_mac_vm_get(int index) {
    if (index < 0 || index >= g_vm_count) return NULL;
    return &g_vms[index];
}

AsbVmMac *asb_mac_vm_find(const char *name) {
    int idx = vm_index_of(name);
    return idx >= 0 ? &g_vms[idx] : NULL;
}

/* ---- Agent resources ---- */

static NSString *agent_resource_directory(void) {
    NSFileManager *fm = [NSFileManager defaultManager];

    /* 1. Bundled: <App>/Contents/Resources/agent_mac */
    NSString *bundle = [[NSBundle mainBundle].resourcePath
                          stringByAppendingPathComponent:@"agent_mac"];
    if (bundle &&
        [fm fileExistsAtPath:[bundle stringByAppendingPathComponent:@"appsandbox-agent"]]) {
        return bundle;
    }

    /* 2. Dev fallback: walk up from bundle to find tools/agent_mac with
     *    a built binary under build/. */
    NSString *cur = [NSBundle mainBundle].bundlePath;
    for (int i = 0; i < 6 && cur.length > 1; i++) {
        NSString *candidate = [cur stringByAppendingPathComponent:@"tools/agent_mac"];
        NSString *bin = [candidate stringByAppendingPathComponent:@"build/appsandbox-agent"];
        if ([fm fileExistsAtPath:bin]) return candidate;
        cur = [cur stringByDeletingLastPathComponent];
    }
    return nil;
}

/* ---- Agent lifecycle ---- */

static void stop_ssh_proxy_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    VmSshProxyMac *proxy = g_ssh_proxy_refs[idx];
    if (!proxy) return;
    [proxy stop];
    g_ssh_proxy_refs[idx] = nil;
    g_vms[idx].ssh_proxy = nil;
    g_vms[idx].ssh_state = 0;
}

static void stop_clipboard_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    VmClipboardMac *clip = g_clipboard_refs[idx];
    if (!clip) return;
    [clip stop];
    g_clipboard_refs[idx] = nil;
}

static void start_clipboard_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (g_clipboard_refs[idx]) return;
    VzVm *vzvm = g_vz_refs[idx];
    if (!vzvm || !vzvm.machine) return;
    VZVirtioSocketDevice *vsock = nil;
    for (id d in vzvm.machine.socketDevices) {
        if ([d isKindOfClass:[VZVirtioSocketDevice class]]) { vsock = d; break; }
    }
    if (!vsock) return;
    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    VmClipboardMac *clip = [[VmClipboardMac alloc] initWithName:nsName
                                                    socketDevice:vsock];
    clip.onLog = ^(NSString *line) {
        post_diag("[%s] clipboard: %s", nsName.UTF8String, line.UTF8String);
    };
    g_clipboard_refs[idx] = clip;
    [clip start];
}

static void stop_agent_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    stop_clipboard_for(idx);
    stop_ssh_proxy_for(idx);
    VmAgentMac *agent = g_agent_refs[idx];
    if (!agent) return;
    [agent stop];
    g_agent_refs[idx] = nil;
    g_vms[idx].agent = nil;
    if (g_vms[idx].agent_online) {
        g_vms[idx].agent_online = NO;
        post_event(CORE_VM_EVENT_AGENT_STATUS, g_vms[idx].name, 0, NULL);
    }
}

static void start_ssh_proxy_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (!g_vms[idx].ssh_enabled) return;
    if (g_ssh_proxy_refs[idx]) return;

    VzVm *vzvm = g_vz_refs[idx];
    if (!vzvm || !vzvm.machine) return;

    VZVirtioSocketDevice *vsock = nil;
    for (id d in vzvm.machine.socketDevices) {
        if ([d isKindOfClass:[VZVirtioSocketDevice class]]) { vsock = d; break; }
    }
    if (!vsock) return;

    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    VmSshProxyMac *proxy = [[VmSshProxyMac alloc] initWithName:nsName
                                                   socketDevice:vsock
                                                    initialPort:g_vms[idx].ssh_port];
    proxy.onPortAssigned = ^(int port) {
        int i = vm_index_of(nsName.UTF8String);
        if (i < 0) return;
        if (g_vms[i].ssh_port != port) {
            g_vms[i].ssh_port = port;
            save_vm_list();
        }
        post_log("[%s] SSH proxy listening on 127.0.0.1:%d", g_vms[i].name, port);
        post_event(CORE_VM_EVENT_AGENT_STATUS, g_vms[i].name, 1, NULL);
        post_list_changed();
    };
    proxy.onLog = ^(NSString *line) {
        post_diag("[%s] ssh: %s", nsName.UTF8String, line.UTF8String);
    };
    g_ssh_proxy_refs[idx] = proxy;
    g_vms[idx].ssh_proxy = proxy;
    [proxy start];
}

static void start_agent_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (g_agent_refs[idx]) return;
    VzVm *vzvm = g_vz_refs[idx];
    if (!vzvm || !vzvm.machine) return;
    NSArray *devs = vzvm.machine.socketDevices;
    VZVirtioSocketDevice *vsock = nil;
    for (id d in devs) {
        if ([d isKindOfClass:[VZVirtioSocketDevice class]]) { vsock = d; break; }
    }
    if (!vsock) {
        post_log("[%s] No VZVirtioSocketDevice on VM; agent not started", g_vms[idx].name);
        return;
    }

    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    VmAgentMac *agent = [[VmAgentMac alloc] initWithName:nsName
                                            socketDevice:vsock];
    agent.sshEnabled = g_vms[idx].ssh_enabled;
    agent.onOnlineChange = ^(BOOL online) {
        int i = vm_index_of(nsName.UTF8String);
        if (i < 0) return;
        g_vms[i].agent_online = online;
        if (online) g_vms[i].agent_last_heartbeat_ms = 0;
        post_event(CORE_VM_EVENT_AGENT_STATUS, g_vms[i].name, online ? 1 : 0, NULL);
        post_list_changed();

        /* Mirror the Windows behavior: first successful agent connection
         * is the strong signal that install actually worked end-to-end. */
        if (online && !g_vms[i].install_complete) {
            g_vms[i].install_complete = YES;
            save_vm_list();
            post_log("[%s] Install complete (agent reached).", g_vms[i].name);
        }

        /* Clipboard is always-on — start when the agent comes up. */
        if (online) start_clipboard_for(i);
        else        stop_clipboard_for(i);
    };
    agent.onSshStateChange = ^(int state) {
        int i = vm_index_of(nsName.UTF8String);
        if (i < 0) return;
        g_vms[i].ssh_state = state;
        post_event(CORE_VM_EVENT_AGENT_STATUS, g_vms[i].name, state, NULL);
        post_list_changed();
        if (state == 2) start_ssh_proxy_for(i);
    };
    agent.onLog = ^(NSString *line) {
        post_diag("agent: %s", line.UTF8String);
    };

    g_agent_refs[idx] = agent;
    g_vms[idx].agent = agent;
    [agent start];
}

/* ---- State change handling ---- */

static void handle_vm_state_change(int idx, VZVirtualMachineState state) {
    if (idx < 0 || idx >= g_vm_count) return;

    static const char *state_names[] = {
        [VZVirtualMachineStateStopped]   = "Stopped",
        [VZVirtualMachineStateRunning]   = "Running",
        [VZVirtualMachineStatePaused]    = "Paused",
        [VZVirtualMachineStateError]     = "Error",
        [VZVirtualMachineStateStarting]  = "Starting",
        [VZVirtualMachineStatePausing]   = "Pausing",
        [VZVirtualMachineStateResuming]  = "Resuming",
        [VZVirtualMachineStateStopping]  = "Stopping",
        [VZVirtualMachineStateSaving]    = "Saving",
        [VZVirtualMachineStateRestoring] = "Restoring",
    };
    const char *label = ((int)state >= 0 && (int)state < (int)(sizeof(state_names)/sizeof(state_names[0])))
        ? state_names[(int)state] : NULL;
    if (label)
        post_log("[%s] State: %s", g_vms[idx].name, label);
    else
        post_log("[%s] State: %d", g_vms[idx].name, (int)state);

    if (state == VZVirtualMachineStateStopping) {
        g_vms[idx].shutting_down = YES;
        post_list_changed();
    } else if (state == VZVirtualMachineStateStopped) {
        g_vms[idx].shutting_down = NO;
        g_vms[idx].running = NO;

        stop_agent_for(idx);

        if (g_display_refs[idx]) {
            VzDisplayWindow *display = g_display_refs[idx];
            [display.window close];
            g_display_refs[idx] = nil;
            g_vms[idx].display = nil;
        }

        g_vz_refs[idx] = nil;
        g_vms[idx].vz_handle = nil;

        post_event(CORE_VM_EVENT_STATE_CHANGED, g_vms[idx].name, 0, NULL);
        post_list_changed();
    } else if (state == VZVirtualMachineStateRunning) {
        g_vms[idx].shutting_down = NO;
        g_vms[idx].running = YES;

        if (g_vms[idx].vz_handle && !g_display_refs[idx]) {
            VzDisplayWindow *display = [[VzDisplayWindow alloc] initWithVzVm:g_vms[idx].vz_handle];
            g_display_refs[idx] = display;
            g_vms[idx].display = display;
            [display showDisplay];
        }

        start_agent_for(idx);

        post_event(CORE_VM_EVENT_STATE_CHANGED, g_vms[idx].name, 1, NULL);
        post_list_changed();
    }
}

/* ---- Install flow ---- */

static void update_install_progress(int idx, double frac, NSString *stage) {
    if (idx < 0 || idx >= g_vm_count) return;
    int pct = (int)(frac * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_vms[idx].install_progress = pct;
    if (stage) {
        strlcpy(g_vms[idx].install_status, [stage UTF8String], sizeof(g_vms[idx].install_status));
    }
    post_event(CORE_VM_EVENT_PROGRESS, g_vms[idx].name, pct, NULL);
    if (stage) {
        post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[idx].name, pct, [stage UTF8String]);
    }
    post_list_changed();
}

static void finish_install(int idx, NSError *error) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (error) {
        g_vms[idx].install_progress = -1;
        g_vms[idx].install_status[0] = '\0';
        post_log("[%s] Install failed: %s", g_vms[idx].name,
                 error.localizedDescription.UTF8String);
        post_alert(g_vms[idx].name, "Install failed: %s",
                   error.localizedDescription.UTF8String);
        post_list_changed();
        return;
    }

    /* macOS install succeeded, but we haven't staged the agent yet.
     * Leave install_complete = NO so the UI's Start guard blocks the user
     * from trying to start while hdiutil has the disk attached for stage.
     * install_complete flips to YES in the stage completion below. */
    strlcpy(g_vms[idx].install_status, "Staging guest agent",
            sizeof(g_vms[idx].install_status));
    post_log("[%s] macOS install complete; staging guest agent...", g_vms[idx].name);
    post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[idx].name, 100, "Staging guest agent");
    post_event(CORE_VM_EVENT_PROGRESS, g_vms[idx].name, 100, NULL);

    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    NSURL *diskURL = [VmDir diskImageURLFor:nsName];
    NSString *agentDir = agent_resource_directory();
    if (!agentDir) {
        post_log("[%s] Agent resources not found; skipping agent stage", g_vms[idx].name);
        g_vms[idx].install_complete = YES;
        save_vm_list();
        post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[idx].name, 100, "Install complete");
        post_list_changed();
        return;
    }

    NSString *adminUser = [NSString stringWithUTF8String:g_vms[idx].admin_user];
    NSString *adminPass = [NSString stringWithUTF8String:g_vms[idx].admin_pass];

    BOOL sshEnabled = g_vms[idx].ssh_enabled;

    [IsoPatchMac stageAgentIntoDiskAtURL:diskURL
                        agentResourceDir:agentDir
                               adminUser:adminUser
                               adminPass:adminPass
                             sshEnabled:sshEnabled
                                progress:^(double frac, NSString *step) {
        (void)frac;
        int i = vm_index_of(nsName.UTF8String);
        if (i >= 0 && step.length) {
            strlcpy(g_vms[i].install_status, step.UTF8String,
                    sizeof(g_vms[i].install_status));
            post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[i].name, 100, step.UTF8String);
        }
    }
                              completion:^(NSError * _Nullable stageErr) {
        int i = vm_index_of(nsName.UTF8String);
        if (i < 0) return;
        if (stageErr) {
            post_log("[%s] Guest agent stage failed: %s",
                     g_vms[i].name, stageErr.localizedDescription.UTF8String);
            post_alert(g_vms[i].name, "Guest agent stage failed: %s",
                       stageErr.localizedDescription.UTF8String);
        } else {
            post_log("[%s] Guest agent staged.", g_vms[i].name);
        }
        /* Zero out the in-memory password buffer now that stage is done
         * (matches Windows' SecureZeroMemory behavior). */
        memset(g_vms[i].admin_pass, 0, sizeof(g_vms[i].admin_pass));
        /* Flip install_complete regardless of stage outcome — the VM is
         * usable either way; a stage failure just means no agent. */
        g_vms[i].install_complete = YES;
        strlcpy(g_vms[i].install_status, "Install complete",
                sizeof(g_vms[i].install_status));
        save_vm_list();
        post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[i].name, 100, "Install complete");
        post_list_changed();
    }];

    post_list_changed();
}

static void start_install_flow(int idx, NSURL *restoreURL) {
    int ramMb = g_vms[idx].ram_mb;
    int hddGb = g_vms[idx].hdd_gb;
    int cpus  = g_vms[idx].cpu_cores;
    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    NSURL *vmDir = [VmDir directoryForVm:nsName];

    post_log("[%s] Starting macOS install (%d cores, %d MB RAM, %d GB disk)",
             nsName.UTF8String, cpus, ramMb, hddGb);
    post_event(CORE_VM_EVENT_INSTALL_STATUS, nsName.UTF8String, 0, "Starting install");
    post_list_changed();

    [IsoPatchMac installMacOSWithName:nsName
                                vmDir:vmDir
                              ipswURL:restoreURL
                                ramMb:ramMb
                                 cpus:cpus
                               diskGb:hddGb
                             progress:^(double frac, NSString *stage) {
        int i = vm_index_of(nsName.UTF8String);
        if (i >= 0) update_install_progress(i, frac, stage);
    }
                           completion:^(NSError * _Nullable err) {
        int i = vm_index_of(nsName.UTF8String);
        if (i >= 0) finish_install(i, err);
    }];
}

/* ---- Public: lifecycle ---- */

int asb_mac_vm_create(const char *name, const char *os_type,
                       int ram_mb, int hdd_gb, int cpu_cores,
                       int gpu_mode, int network_mode,
                       const char *image_path,
                       const char *admin_user,
                       const char *admin_pass,
                       BOOL ssh_enabled) {
    if (!name || !os_type) return BACKEND_ERR_INVALID_ARG;
    if (vm_index_of(name) >= 0) {
        post_alert(name, "A VM named '%s' already exists", name);
        return BACKEND_ERR_INVALID_ARG;
    }
    if (g_vm_count >= ASB_MAX_VMS) {
        post_alert(name, "Maximum number of VMs reached");
        return BACKEND_ERR_FAILED;
    }

    /* Prompt for admin up front so the user isn't blocked 20 minutes into
     * the install. Token is cached for the process lifetime; subsequent
     * VM creations reuse it silently. */
    NSError *authErr = nil;
    if (![IsoPatchMac preauthorize:&authErr]) {
        post_alert(name, "Admin authorization required to create VM: %s",
                   authErr.localizedDescription.UTF8String ?: "user cancelled");
        return BACKEND_ERR_FAILED;
    }

    int idx = g_vm_count;
    AsbVmMac *vm = &g_vms[idx];
    memset(vm, 0, sizeof(*vm));
    strlcpy(vm->name, name, sizeof(vm->name));
    strlcpy(vm->os_type, os_type, sizeof(vm->os_type));
    vm->ram_mb = ram_mb > 0 ? ram_mb : 8192;
    vm->hdd_gb = hdd_gb > 0 ? hdd_gb : 64;
    vm->cpu_cores = cpu_cores > 0 ? cpu_cores : 4;
    vm->gpu_mode = gpu_mode;
    vm->network_mode = network_mode;
    strlcpy(vm->admin_user,
            (admin_user && admin_user[0]) ? admin_user : "user",
            sizeof(vm->admin_user));
    strlcpy(vm->admin_pass,
            (admin_pass && admin_pass[0]) ? admin_pass : "test123",
            sizeof(vm->admin_pass));
    vm->ssh_enabled = ssh_enabled;
    vm->install_progress = 0;
    strlcpy(vm->install_status, "Preparing", sizeof(vm->install_status));
    g_vm_count++;
    save_vm_list();

    NSString *nsName = [NSString stringWithUTF8String:name];

    NSError *dirErr = nil;
    if (![VmDir ensureDirectoryFor:nsName error:&dirErr]) {
        post_alert(name, "Failed to create VM directory: %s",
                   dirErr.localizedDescription.UTF8String);
        return BACKEND_ERR_FAILED;
    }

    NSString *imagePath = (image_path && image_path[0])
        ? [NSString stringWithUTF8String:image_path] : nil;

    if (imagePath.length > 0) {
        if (image_path) strlcpy(g_last_ipsw_path, image_path, sizeof(g_last_ipsw_path));
        run_on_main(^{
            int i = vm_index_of(nsName.UTF8String);
            if (i >= 0) start_install_flow(i, [NSURL fileURLWithPath:imagePath]);
        });
        return BACKEND_OK;
    }

    NSURL *cachedIpsw = [[[VmDir vmsRootDirectory] URLByDeletingLastPathComponent]
                            URLByAppendingPathComponent:@"restore.ipsw"];

    if ([[NSFileManager defaultManager] fileExistsAtPath:cachedIpsw.path]) {
        post_log("Using cached restore image: %s", cachedIpsw.path.UTF8String);
        run_on_main(^{
            int i = vm_index_of(nsName.UTF8String);
            if (i >= 0) start_install_flow(i, cachedIpsw);
        });
        return BACKEND_OK;
    }

    post_log("No cached restore image found, fetching latest from Apple...");
    post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[idx].name, 0, "Fetching latest restore image");

    [IsoPatchMac fetchLatestIpswToURL:cachedIpsw
                              progress:^(double frac, NSString *stage) {
        int i = vm_index_of(nsName.UTF8String);
        if (i >= 0) update_install_progress(i, frac, stage);
    }
                            completion:^(NSError * _Nullable dlErr) {
        if (dlErr) {
            post_log("Fetch failed: %s", dlErr.localizedDescription.UTF8String);
            int i = vm_index_of(nsName.UTF8String);
            if (i >= 0) finish_install(i, dlErr);
            return;
        }
        post_log("Restore image downloaded, starting install...");
        int i = vm_index_of(nsName.UTF8String);
        if (i >= 0) start_install_flow(i, cachedIpsw);
    }];

    return BACKEND_OK;
}

int asb_mac_vm_start(const char *name) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;
    if (!g_vms[idx].install_complete) {
        post_alert(name, "Cannot start: install has not completed");
        return BACKEND_ERR_FAILED;
    }
    if (g_vms[idx].running) {
        post_alert(name, "VM is already running");
        return BACKEND_ERR_ALREADY_RUNNING;
    }

    NSString *nsName = [NSString stringWithUTF8String:name];
    post_log("[%s] Loading VM configuration...", name);

    NSError *err = nil;
    VzVm *vm = [VzVm loadVmNamed:nsName
                            ramMb:g_vms[idx].ram_mb
                         cpuCores:g_vms[idx].cpu_cores
                            error:&err];
    if (!vm) {
        post_log("[%s] Load failed: %s", name,
                 err ? err.localizedDescription.UTF8String : "unknown");
        post_alert(name, "Load failed: %s",
                   err ? err.localizedDescription.UTF8String : "unknown");
        return BACKEND_ERR_FAILED;
    }

    g_vz_refs[idx] = vm;
    g_vms[idx].vz_handle = vm;

    vm.onStateChange = ^(VZVirtualMachineState state) {
        run_on_main(^{
            int i = vm_index_of(nsName.UTF8String);
            if (i >= 0) handle_vm_state_change(i, state);
        });
    };

    [vm startWithCompletion:^(NSError * _Nullable startErr) {
        run_on_main(^{
            if (startErr) {
                int i = vm_index_of(nsName.UTF8String);
                if (i >= 0) {
                    g_vz_refs[i] = nil;
                    g_vms[i].vz_handle = nil;
                    post_log("[%s] Start failed: %s", g_vms[i].name,
                             startErr.localizedDescription.UTF8String);
                    post_alert(g_vms[i].name, "Start failed: %s",
                               startErr.localizedDescription.UTF8String);
                    post_event(CORE_VM_EVENT_STATE_CHANGED, g_vms[i].name, 0, NULL);
                    post_list_changed();
                }
            }
        });
    }];

    return BACKEND_OK;
}

int asb_mac_vm_stop(const char *name, int force) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;

    VzVm *vm = g_vms[idx].vz_handle;
    if (!vm) return BACKEND_ERR_NOT_RUNNING;

    NSString *nsName = [NSString stringWithUTF8String:name];
    void (^onError)(NSError *) = ^(NSError * _Nullable stopErr) {
        if (!stopErr) return;
        run_on_main(^{
            int i = vm_index_of(nsName.UTF8String);
            if (i >= 0) {
                post_log("[%s] Stop failed: %s", g_vms[i].name,
                         stopErr.localizedDescription.UTF8String);
                post_alert(g_vms[i].name, "Stop failed: %s",
                           stopErr.localizedDescription.UTF8String);
            }
        });
    };

    if (force) {
        [vm stopWithCompletion:onError];
    } else {
        [vm requestStopWithCompletion:onError];
    }

    return BACKEND_OK;
}

int asb_mac_vm_delete(const char *name) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;

    if (g_vms[idx].running && g_vms[idx].vz_handle) {
        post_log("[%s] Stopping VM before delete...", name);
        [g_vms[idx].vz_handle stopWithCompletion:^(NSError * _Nullable err) { (void)err; }];
        g_vms[idx].running = NO;
        g_vms[idx].shutting_down = NO;
        if (g_display_refs[idx]) {
            VzDisplayWindow *display = g_display_refs[idx];
            [display.window close];
        }
    }

    post_log("[%s] Deleting VM...", name);

    NSString *nsName = [NSString stringWithUTF8String:name];
    NSError *err = nil;
    if (![VmDir deleteVm:nsName error:&err]) {
        post_log("[%s] Delete failed: %s", name, err.localizedDescription.UTF8String);
        post_alert(name, "Delete failed: %s", err.localizedDescription.UTF8String);
        return BACKEND_ERR_FAILED;
    }

    stop_agent_for(idx);
    g_vz_refs[idx] = nil;
    g_display_refs[idx] = nil;

    for (int i = idx; i < g_vm_count - 1; i++) {
        g_vms[i] = g_vms[i + 1];
        g_vz_refs[i] = g_vz_refs[i + 1];
        g_display_refs[i] = g_display_refs[i + 1];
        g_agent_refs[i] = g_agent_refs[i + 1];
        g_ssh_proxy_refs[i] = g_ssh_proxy_refs[i + 1];
        g_clipboard_refs[i] = g_clipboard_refs[i + 1];
    }
    g_vm_count--;
    memset(&g_vms[g_vm_count], 0, sizeof(AsbVmMac));
    g_vz_refs[g_vm_count] = nil;
    g_display_refs[g_vm_count] = nil;
    g_agent_refs[g_vm_count] = nil;
    g_ssh_proxy_refs[g_vm_count] = nil;
    g_clipboard_refs[g_vm_count] = nil;

    save_vm_list();
    post_log("[%s] VM deleted", name);
    post_list_changed();
    return BACKEND_OK;
}

/* ---- Public: config editing ---- */

int asb_mac_vm_edit(const char *name, const char *field, const char *value) {
    if (!name || !field || !value) return BACKEND_ERR_INVALID_ARG;
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;
    if (g_vms[idx].running) return BACKEND_ERR_ALREADY_RUNNING;

    if (strcmp(field, "name") == 0) {
        if (vm_index_of(value) >= 0) return BACKEND_ERR_INVALID_ARG;
        NSString *oldName = [NSString stringWithUTF8String:g_vms[idx].name];
        NSString *newName = [NSString stringWithUTF8String:value];
        NSURL *oldDir = [VmDir directoryForVm:oldName];
        NSURL *newDir = [VmDir directoryForVm:newName];
        NSError *err = nil;
        if (![[NSFileManager defaultManager] moveItemAtURL:oldDir toURL:newDir error:&err]) {
            post_alert(name, "Rename failed: %s", err.localizedDescription.UTF8String);
            return BACKEND_ERR_FAILED;
        }
        strlcpy(g_vms[idx].name, value, sizeof(g_vms[idx].name));
    } else if (strcmp(field, "ramMb") == 0) {
        g_vms[idx].ram_mb = atoi(value);
    } else if (strcmp(field, "cpuCores") == 0) {
        g_vms[idx].cpu_cores = atoi(value);
    } else if (strcmp(field, "gpuMode") == 0) {
        g_vms[idx].gpu_mode = atoi(value);
    } else if (strcmp(field, "networkMode") == 0) {
        g_vms[idx].network_mode = atoi(value);
    } else {
        return BACKEND_ERR_INVALID_ARG;
    }

    save_vm_list();
    post_event(CORE_VM_EVENT_STATE_CHANGED, g_vms[idx].name, 0, NULL);
    post_list_changed();
    return BACKEND_OK;
}

/* ---- Public: persistence / callbacks ---- */

void asb_mac_save(void) {
    save_vm_list();
}

void asb_mac_set_event_cb(AsbMacEventCallback cb) {
    g_event_cb = cb;
}
