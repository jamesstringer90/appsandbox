/*
 * asb_core_mac.m -- macOS orchestrator implementation.
 *
 * Mirrors asb_core.c on Windows: owns the in-memory VM array (g_vms[]),
 * persists to ~/Library/Application Support/AppSandbox/vms.cfg in the
 * same INI format as Windows, and drives VM lifecycle through the VZ
 * helper modules (vz_vm, vz_install, vz_display, vz_disk, vz_network).
 */

#import "asb_core_mac.h"
#import <CoreGraphics/CoreGraphics.h>   /* CGSessionCopyCurrentDictionary (A-gate) */
#import "vm_dir.h"
#import "vz_vm.h"
#import "vz_display.h"
#import "vz_network.h"
#import "vm_agent_mac.h"
#import "vm_ssh_proxy_mac.h"
#import "vm_clipboard_mac.h"
#import "iso_patch_mac.h"
#import "host_info.h"
#import "qemu_vm.h"                 /* Windows guest: launch via QEMU+HVF instead of VZ */
#import "asb_ivshmem_transport.h"  /* Windows guest: channel helpers ride ivshmem, not vsock */
#import "idd_display.h"            /* Windows guest: host display window over ivshmem ch2+ch3 */

#include "asb_types.h"

#include <stdio.h>
#include <string.h>

/* ---- Global state ---- */

static AsbVmMac g_vms[ASB_MAX_VMS];
static int g_vm_count = 0;
static char g_last_ipsw_path[1024] = {0};
static AsbMacEventCallback g_event_cb = NULL;

/* Headless daemon mode: suppress the per-VM display window, the clipboard
 * channel, and the VM audio devices (see asb_mac_set_headless in the header). */
static BOOL g_headless = NO;

/* Strong references to ObjC objects whose lifetime is tied to g_vms[].
 * The struct stores __unsafe_unretained pointers; these arrays keep them alive. */
static id g_vz_refs[ASB_MAX_VMS];
static id g_display_refs[ASB_MAX_VMS];
static id g_agent_refs[ASB_MAX_VMS];
static id g_ssh_proxy_refs[ASB_MAX_VMS];
static id g_clipboard_refs[ASB_MAX_VMS];
/* Windows-guest backend: QemuVm (the VzVm peer) + its ivshmem transport, per VM. */
static id g_qemu_refs[ASB_MAX_VMS];
static id g_transport_refs[ASB_MAX_VMS];

/* A Windows guest is launched via QEMU+ivshmem; macOS (and future Linux) use VZ. */
static BOOL vm_is_windows_idx(int idx) {
    return idx >= 0 && idx < g_vm_count &&
           strcasecmp(g_vms[idx].os_type, "Windows") == 0;
}

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

/* ---- AppSandbox SSH keypair (mirrors ensure_appsandbox_ssh_key on Windows) ---- */

NSString *asb_mac_ssh_key_path(void) {
    NSURL *root = [VmDir vmsRootDirectory];   /* .../AppSandbox/VMs */
    return [[[root URLByDeletingLastPathComponent]
                URLByAppendingPathComponent:@"ssh/id_appsandbox"] path];
}

/* Ensure the AppSandbox ed25519 keypair exists (generating it via the system
 * ssh-keygen if absent) and return its public-key line, newline-trimmed.
 * ed25519 keeps the line short enough for the agents' command buffers; the
 * private key stays host-side for `ssh -i`. Returns NO on any failure, and
 * never leaves partial key material behind to be trusted on a later run. */
static BOOL ensure_appsandbox_ssh_key(char *pubkey_out, size_t cap) {
    if (cap > 0) pubkey_out[0] = '\0';
    NSString *priv = asb_mac_ssh_key_path();
    NSString *pub  = [priv stringByAppendingString:@".pub"];
    NSFileManager *fm = [NSFileManager defaultManager];

    if (![fm fileExistsAtPath:pub]) {
        [fm createDirectoryAtPath:[priv stringByDeletingLastPathComponent]
      withIntermediateDirectories:YES attributes:nil error:nil];
        /* A leftover private half would make ssh-keygen prompt interactively. */
        [fm removeItemAtPath:priv error:nil];

        NSTask *t = [[NSTask alloc] init];
        t.launchPath = @"/usr/bin/ssh-keygen";
        t.arguments  = @[@"-t", @"ed25519", @"-f", priv, @"-N", @"", @"-C", @"appsandbox", @"-q"];
        NSError *err = nil;
        if (![t launchAndReturnError:&err]) {
            post_log("ssh key: ssh-keygen launch failed: %s",
                     err.localizedDescription.UTF8String ?: "?");
            return NO;
        }
        [t waitUntilExit];
        if (t.terminationStatus != 0 || ![fm fileExistsAtPath:pub]) {
            post_log("ssh key: ssh-keygen failed (exit %d)", t.terminationStatus);
            [fm removeItemAtPath:priv error:nil];
            [fm removeItemAtPath:pub error:nil];
            return NO;
        }
        post_log("ssh key: generated AppSandbox keypair at %s", priv.UTF8String);
    }

    NSString *line = [NSString stringWithContentsOfFile:pub
                                               encoding:NSUTF8StringEncoding error:nil];
    line = [line stringByTrimmingCharactersInSet:
                [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!line.length) {
        post_log("ssh key: cannot read public key %s", pub.UTF8String);
        return NO;
    }
    strlcpy(pubkey_out, line.UTF8String, cap);
    return pubkey_out[0] != '\0';
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
        if (g_vms[i].disk_directory[0])
            fprintf(f, "DiskDirectory=%s\n", g_vms[i].disk_directory);
        fprintf(f, "RamMB=%d\n", g_vms[i].ram_mb);
        fprintf(f, "HddGB=%d\n", g_vms[i].hdd_gb);
        fprintf(f, "CpuCores=%d\n", g_vms[i].cpu_cores);
        fprintf(f, "GpuMode=%d\n", g_vms[i].gpu_mode);
        fprintf(f, "NetworkMode=%d\n", g_vms[i].network_mode);
        if (g_vms[i].test_mode)
            fprintf(f, "TestMode=1\n");
        if (g_vms[i].admin_user[0])
            fprintf(f, "AdminUser=%s\n", g_vms[i].admin_user);
        /* admin_pass intentionally NOT persisted — matches Windows. */
        if (g_vms[i].ssh_enabled)
            fprintf(f, "SshEnabled=1\n");
        if (g_vms[i].ssh_port > 0)
            fprintf(f, "SshPort=%d\n", g_vms[i].ssh_port);
        if (g_vms[i].ssh_deploy_key)
            fprintf(f, "SshDeployKey=1\n");
        if (g_vms[i].ssh_pubkey[0])
            fprintf(f, "SshPubKey=%s\n", g_vms[i].ssh_pubkey);
        if (g_vms[i].install_complete)
            fprintf(f, "InstallComplete=1\n");
        if (g_vms[i].disk_built)
            fprintf(f, "DiskBuilt=1\n");
        fprintf(f, "\n");
    }

    fclose(f);
}

static void load_vm_list(void) {
    NSURL *url = config_file_url();
    if (!url) return;
    FILE *f = fopen(url.fileSystemRepresentation, "r");
    if (!f) return;

    char line[2048];
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
        else if (strncmp(line, "DiskDirectory=", 14) == 0)
            strlcpy(vm->disk_directory, line + 14, sizeof(vm->disk_directory));
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
        else if (strncmp(line, "TestMode=", 9) == 0)
            vm->test_mode = (atoi(line + 9) != 0);
        else if (strncmp(line, "AdminUser=", 10) == 0)
            strlcpy(vm->admin_user, line + 10, sizeof(vm->admin_user));
        else if (strncmp(line, "SshEnabled=", 11) == 0)
            vm->ssh_enabled = (atoi(line + 11) != 0);
        else if (strncmp(line, "SshPort=", 8) == 0)
            vm->ssh_port = atoi(line + 8);
        else if (strncmp(line, "SshDeployKey=", 13) == 0)
            vm->ssh_deploy_key = (atoi(line + 13) != 0);
        else if (strncmp(line, "SshPubKey=", 10) == 0)
            strlcpy(vm->ssh_pubkey, line + 10, sizeof(vm->ssh_pubkey));
        else if (strncmp(line, "InstallComplete=", 16) == 0) {
            vm->install_complete = (atoi(line + 16) != 0);
            /* Migration: configs written before disk_built existed only stored
               InstallComplete (which then meant "disk built"). A provisioned VM is
               necessarily built, so seed disk_built too — otherwise an existing VM
               would fail the start gate. An explicit DiskBuilt line reaffirms it. */
            if (vm->install_complete) vm->disk_built = YES;
        }
        else if (strncmp(line, "DiskBuilt=", 10) == 0)
            vm->disk_built = (atoi(line + 10) != 0);
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
    memset(g_qemu_refs, 0, sizeof(g_qemu_refs));
    memset(g_transport_refs, 0, sizeof(g_transport_refs));
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
        /* Windows guest: QEMU is an external child process, so (unlike a VZ
         * guest, whose in-process VZVirtualMachine dies when its ref is
         * released below) it must be told to exit or it orphans and keeps the
         * instance lock. [stop] sends HMP `quit` over the loopback monitor
         * (works whether QEMU runs as us or elevated), terminating it. */
        if (g_qemu_refs[i]) [(QemuVm *)g_qemu_refs[i] stop];
        g_qemu_refs[i] = nil;
        if (g_transport_refs[i]) [(AsbIvshmemTransport *)g_transport_refs[i] close];
        g_transport_refs[i] = nil;
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
    /* Headless: NSPasteboard is per-Aqua-session and the channel both reads
       and serves the host pasteboard -- a daemon must not touch it. */
    if (g_headless) return;
    if (g_clipboard_refs[idx]) return;
    if (vm_is_windows_idx(idx)) return;   /* Windows clipboard over ivshmem: wired in P3 */
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

    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    VmSshProxyMac *proxy;
    if (vm_is_windows_idx(idx)) {
        AsbIvshmemTransport *t = g_transport_refs[idx];
        if (!t) return;
        proxy = [[VmSshProxyMac alloc] initWithName:nsName
                                   ivshmemTransport:t
                                        initialPort:g_vms[idx].ssh_port];
    } else {
        VzVm *vzvm = g_vz_refs[idx];
        if (!vzvm || !vzvm.machine) return;
        VZVirtioSocketDevice *vsock = nil;
        for (id d in vzvm.machine.socketDevices) {
            if ([d isKindOfClass:[VZVirtioSocketDevice class]]) { vsock = d; break; }
        }
        if (!vsock) return;
        proxy = [[VmSshProxyMac alloc] initWithName:nsName
                                       socketDevice:vsock
                                        initialPort:g_vms[idx].ssh_port];
    }
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

/* Open the Windows guest's IDD display window. Trigger is agent-online (VDD/IDD up), not QEMU-running.
 * Main-queue only (NSWindow). No-op: headless, non-Windows, already open, or no transport yet. */
static void open_idd_display_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (g_headless) return;
    if (!vm_is_windows_idx(idx)) return;
    if (g_display_refs[idx]) return;
    AsbIvshmemTransport *t = g_transport_refs[idx];
    if (!t) return;
    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    IddDisplayWindow *display = [[IddDisplayWindow alloc] initWithName:nsName transport:t];
    g_display_refs[idx] = display;
    g_vms[idx].display = (VzDisplayWindow *)display;   /* API-compatible surface (window/userClosed/showDisplay) */
    [display showDisplay];
}

static void start_agent_for(int idx) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (g_agent_refs[idx]) return;

    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    VmAgentMac *agent;
    if (vm_is_windows_idx(idx)) {
        /* Windows guest: reach the agent over ivshmem ch1 (no vsock). */
        AsbIvshmemTransport *t = g_transport_refs[idx];
        if (!t) { post_log("[%s] No ivshmem transport; agent not started", g_vms[idx].name); return; }
        agent = [[VmAgentMac alloc] initWithName:nsName ivshmemTransport:t];
        agent.onIddStatusChange = ^(BOOL ready) {
            int i = vm_index_of(nsName.UTF8String);
            if (i < 0) return;
            g_vms[i].idd_ready = ready;   /* gates display_ready for the Windows guest */
            post_diag("[%s] IDD driver %s", g_vms[i].name, ready ? "ready" : "not ready");
            post_list_changed();
        };
    } else {
        VzVm *vzvm = g_vz_refs[idx];
        if (!vzvm || !vzvm.machine) return;
        VZVirtioSocketDevice *vsock = nil;
        for (id d in vzvm.machine.socketDevices) {
            if ([d isKindOfClass:[VZVirtioSocketDevice class]]) { vsock = d; break; }
        }
        if (!vsock) {
            post_log("[%s] No VZVirtioSocketDevice on VM; agent not started", g_vms[idx].name);
            return;
        }
        agent = [[VmAgentMac alloc] initWithName:nsName socketDevice:vsock];
    }
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

        /* Windows guest: open the IDD display window now (and only now) that the agent reports online —
         * its VDD/IDD driver is up, so ch2 has frames to show. Not opened at QEMU-Running (would be a
         * dark window). Main-queue only; no-op if headless / already open. */
        if (online && vm_is_windows_idx(i)) {
            run_on_main(^{
                int j = vm_index_of(nsName.UTF8String);
                if (j >= 0) open_idd_display_for(j);
            });
        }

        /* Catch-up: the display window may have been opened before the
         * agent was reachable. showDisplay fires once at window-open and
         * bails silently if the agent isn't online yet, which leaves the
         * guest stuck in whatever audio / sync state was persisted from
         * last session. Push the current state now. */
        if (online) {
            VzDisplayWindow *disp = g_display_refs[i];
            if (disp) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    int j = vm_index_of(nsName.UTF8String);
                    if (j < 0) return;
                    NSWindow *w = [(VzDisplayWindow *)g_display_refs[j] window];
                    BOOL windowVisible = w.isVisible && !w.isMiniaturized;
                    BOOL windowKey     = w.isKeyWindow;
                    asb_mac_vm_set_audio_muted(g_vms[j].name, !windowVisible);
                    asb_mac_vm_set_clipboard_sync(g_vms[j].name, windowKey);
                });
            }
        }
    };
    agent.onSshStateChange = ^(int state) {
        int i = vm_index_of(nsName.UTF8String);
        if (i < 0) return;
        g_vms[i].ssh_state = state;
        post_event(CORE_VM_EVENT_AGENT_STATUS, g_vms[i].name, state, NULL);
        post_list_changed();
        if (state == 2) start_ssh_proxy_for(i);
        /* SSH key deploy is driven by the agent itself (fire-and-forget on
           ssh_ready -> agent.onKeyDeployed below), mirroring Windows. */
    };
    agent.onLog = ^(NSString *line) {
        post_diag("agent: %s", line.UTF8String);
    };

    /* SSH key deploy (fire-and-forget on ssh_ready, mirrors Windows). The
       agent sends ssh_deploy_key itself and reports the guest's async reply
       here -- set BEFORE start so it's ready when ssh comes up. */
    if (g_vms[idx].ssh_deploy_key && g_vms[idx].ssh_pubkey[0])
        agent.deployKeyLine = [NSString stringWithUTF8String:g_vms[idx].ssh_pubkey];
    agent.onKeyDeployed = ^(BOOL ok) {
        int i = vm_index_of(nsName.UTF8String);
        if (i < 0) return;
        g_vms[i].ssh_key_deployed = ok;
        post_log("[%s] SSH key %s.", g_vms[i].name, ok ? "deployed" : "deploy FAILED");
        post_event(CORE_VM_EVENT_AGENT_STATUS, g_vms[i].name,
                   g_vms[i].agent_online ? 1 : 0, NULL);
        post_list_changed();
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
        /* Re-deploy on next boot: the guest disk may change while stopped, so
           "deployed" is only valid per boot (the guest write is idempotent).
           Mirrors the Windows reset in asb_hcs_state_changed/asb_vm_stop. */
        g_vms[idx].ssh_key_deployed = NO;

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

        /* Headless: the NSWindow + VZVirtualMachineView is the core's one
           window-server dependency -- skip it entirely. The Stopped/Delete
           teardown paths nil-check g_display_refs, so they become no-ops. */
        if (!g_headless && g_vms[idx].vz_handle && !g_display_refs[idx]) {
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

/* ---- QEMU (Windows guest) state change ---- *
 * The QemuVm peer of handle_vm_state_change: on Running we capture the ivshmem transport and bring
 * the agent up (which, on ssh_ready, starts the ssh proxy + deploys the key — same callbacks as VZ);
 * on Stopped we tear the helpers down and drop the transport. On Running we also open the host display
 * window (IddDisplayWindow over ch2 frames + ch3 input); on Stopped we close it before the transport. */
static void handle_qemu_state_change(int idx, QemuVmState st) {
    if (idx < 0 || idx >= g_vm_count) return;
    if (st == QemuVmStateStopping) {
        g_vms[idx].shutting_down = YES;
        post_list_changed();
    } else if (st == QemuVmStateStopped) {
        g_vms[idx].shutting_down = NO;
        g_vms[idx].running = NO;
        g_vms[idx].idd_ready = NO;
        g_vms[idx].ssh_key_deployed = NO;   /* re-deployed each boot (matches VZ + Windows) */
        stop_agent_for(idx);                 /* also stops the ssh proxy + clipboard */

        /* Tear the display window down (mirrors the VZ Stopped path). g_display_refs holds a
           IddDisplayWindow* here; closing it fires windowWillClose: which joins its ch2/ch3
           reader threads BEFORE the transport below is dropped (it reads them via the fds). */
        if (g_display_refs[idx]) {
            IddDisplayWindow *display = g_display_refs[idx];
            [display.window close];
            g_display_refs[idx] = nil;
            g_vms[idx].display = nil;
        }

        g_transport_refs[idx] = nil;
        g_qemu_refs[idx] = nil;
        post_event(CORE_VM_EVENT_STATE_CHANGED, g_vms[idx].name, 0, NULL);
        post_list_changed();
    } else if (st == QemuVmStateRunning) {
        g_vms[idx].shutting_down = NO;
        g_vms[idx].running = YES;
        QemuVm *q = g_qemu_refs[idx];
        g_transport_refs[idx] = q.transport;   /* the ivshmem transport the channel helpers ride */

        /* NOTE: the IDD display window is NOT opened here. It opens only once the guest agent reports
           online (open_idd_display_for, fired from start_agent_for's onOnlineChange) — the VDD's IDD
           driver isn't up at QEMU-Running, so opening now would just show a dark window. */
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

/* Is the VM's disk image still attached by hdiutil? The stage step attaches it
 * (to mount the APFS Data volume and copy the agent in) and detaches it before
 * exiting, but the kernel device release can lag a beat -- VZ cannot attach the
 * disk for boot until it is fully free, so the auto-start below polls this. */
static BOOL disk_image_attached(NSString *path) {
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = @"/usr/bin/hdiutil";
    t.arguments  = @[@"info", @"-plist"];
    NSPipe *outp = [NSPipe pipe];
    t.standardOutput = outp;
    t.standardError  = [NSPipe pipe];
    if (![t launchAndReturnError:nil]) return NO;
    NSData *d = [outp.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    id pl = [NSPropertyListSerialization propertyListWithData:d options:0 format:NULL error:nil];
    if (![pl isKindOfClass:[NSDictionary class]]) return NO;
    for (NSDictionary *img in pl[@"images"]) {
        if ([img isKindOfClass:[NSDictionary class]] &&
            [img[@"image-path"] isEqualToString:path])
            return YES;
    }
    return NO;
}

/* After install + agent stage, transition the VM straight from "Install
 * complete" into booting -- parity with the Windows create path and the SDK's
 * documented "create() is async and auto-starts". The wait for the disk to be
 * released runs OFF the main queue (so the run loop stays live), then the start
 * is marshalled back to main. No-op if the VM was deleted or already started. */
static void autostart_after_install(NSString *nsName, NSURL *diskURL) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:20.0];
        while (disk_image_attached(diskURL.path) && [deadline timeIntervalSinceNow] > 0)
            usleep(300000);
        dispatch_async(dispatch_get_main_queue(), ^{
            int i = vm_index_of(nsName.UTF8String);
            if (i < 0 || g_vms[i].running) return;
            post_log("[%s] Install complete -- starting VM.", g_vms[i].name);
            asb_mac_vm_start(g_vms[i].name);
        });
    });
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
     * Leave disk_built = NO so the Start guard blocks the user from trying to
     * start while hdiutil has the disk attached for stage. disk_built (and, for
     * macOS, install_complete) flip to YES in the stage completion below. */
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
        /* Wipe the plaintext admin password from memory before returning. */
        memset(g_vms[idx].admin_pass, 0, sizeof(g_vms[idx].admin_pass));
        /* macOS has no separate first-boot install phase — once the disk is built
           it boots straight to ready, so mark both flags now. */
        g_vms[idx].disk_built = YES;
        g_vms[idx].install_complete = YES;
        save_vm_list();
        post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[idx].name, 100, "Install complete");
        post_list_changed();
        autostart_after_install(nsName, diskURL);   /* boot once the disk is free */
        return;
    }

    NSString *adminUser = [NSString stringWithUTF8String:g_vms[idx].admin_user];
    NSString *adminPass = [NSString stringWithUTF8String:g_vms[idx].admin_pass];

    BOOL sshEnabled = g_vms[idx].ssh_enabled;

    [IsoPatchMac stageAgentIntoDiskAtURL:diskURL
                        agentResourceDir:agentDir
                               adminUser:adminUser
                               adminPass:adminPass
                            computerName:nsName
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
        /* Disk is built + staged → bootable. macOS has no separate first-boot
         * install phase, so mark install_complete now too (a stage failure just
         * means no agent; the VM is still usable). */
        g_vms[i].disk_built = YES;
        g_vms[i].install_complete = YES;
        strlcpy(g_vms[i].install_status, "Install complete",
                sizeof(g_vms[i].install_status));
        save_vm_list();
        post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[i].name, 100, "Install complete");
        post_list_changed();
        autostart_after_install(nsName, diskURL);   /* boot once the disk is free */
    }];

    post_list_changed();
}

/* ---- Windows from-scratch create (mount ISO -> our NTFS writer -> stage) ----
 * Mirrors the Windows VHDX-first create path but uses iso-patch-mac build-windows
 * (no DISM). On success the disk boots via QemuVm (testMode); the FirstLogonCommand
 * + SetupComplete.cmd we staged bring the agent + drivers online. */
static void start_windows_build_flow(int idx, NSURL *isoURL) {
    NSString *nsName = [NSString stringWithUTF8String:g_vms[idx].name];
    NSURL *diskURL = [VmDir diskImageURLFor:nsName];
    int diskGb = g_vms[idx].hdd_gb;

    /* Capture per-VM state on the main queue (g_vms is main-owned); the blocking downloads + the
       disk build run on a background queue. */
    BOOL sshEnabled     = g_vms[idx].ssh_enabled;
    BOOL testMode       = g_vms[idx].test_mode;
    NSString *adminUser = [NSString stringWithUTF8String:g_vms[idx].admin_user];
    NSString *adminPass = [NSString stringWithUTF8String:g_vms[idx].admin_pass];

    post_log("[%s] Caching signed guest payload (agents + drivers) + OpenSSH...", g_vms[idx].name);
    post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[idx].name, 0, "Downloading guest payload");
    post_list_changed();

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        /* Off-main (blocking; instant on cache hits). The guest payload comes ONLY from the downloaded
           SIGNED release zip (EV-signed agents + attestation-signed drivers incl. ivshmem SHM) -- no
           bundled fallback. NetKVM (virtio-net) is a separate vendored zip; the OpenSSH MSI is separate
           too. All are cached under the support dir like the IPSW. */
        NSString *signedZip = [IsoPatchMac ensureSignedWinPayloadZipCached];
        NSString *netkvmZip = [IsoPatchMac ensureNetkvmZipCached];
        NSString *sshMsi    = sshEnabled ? [IsoPatchMac ensureOpenSSHMsiCached] : nil;

        dispatch_async(dispatch_get_main_queue(), ^{
            int i = vm_index_of(nsName.UTF8String);
            if (i < 0) return;   /* VM deleted while downloading */
            if (!signedZip) {
                post_log("[%s] Signed Windows guest payload download failed -- cannot build (JIT-only, no fallback).",
                         g_vms[i].name);
                post_alert(g_vms[i].name, "Windows guest payload download failed");
                finish_install(i, [NSError errorWithDomain:@"AsbCore" code:1
                    userInfo:@{NSLocalizedDescriptionKey:@"Signed Windows guest payload download failed"}]);
                return;
            }
            if (!netkvmZip)
                post_log("[%s] NetKVM download failed; VM will build without a guest network driver.", g_vms[i].name);
            if (sshEnabled && !sshMsi)
                post_log("[%s] OpenSSH MSI download failed; VM will build without SSH server.", g_vms[i].name);

            post_log("[%s] Building Windows disk from ISO (%d GB) ...", g_vms[i].name, diskGb);
            post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[i].name, 0, "Building Windows disk");
            post_list_changed();

            [IsoPatchMac buildWindowsDiskWithISO:isoURL
                                        outDisk:diskURL
                               signedPayloadZip:signedZip
                                      netkvmZip:netkvmZip
                                     sshMsiPath:sshMsi
                                         vmName:nsName
                                      adminUser:adminUser
                                      adminPass:adminPass
                                           lang:nil
                                         diskGb:diskGb
                                       testMode:testMode
                                       progress:^(double frac, NSString *step) {
                int j = vm_index_of(nsName.UTF8String);
                if (j < 0) return;
                if (frac == ISO_PATCH_PROGRESS_LOG) {   /* e.g. "Detected ISO language: en-US" */
                    post_log("[%s] %s", g_vms[j].name, step.UTF8String);
                    return;
                }
                update_install_progress(j, frac, step);
            }
                                     completion:^(NSError * _Nullable err) {
                int j = vm_index_of(nsName.UTF8String);
                if (j < 0) return;
                /* Wipe the in-memory admin password now that the answer file is written. */
                memset(g_vms[j].admin_pass, 0, sizeof(g_vms[j].admin_pass));
                if (err) {
                    post_log("[%s] Windows disk build failed: %s",
                             g_vms[j].name, err.localizedDescription.UTF8String);
                    post_alert(g_vms[j].name, "Windows disk build failed: %s",
                               err.localizedDescription.UTF8String);
                    g_vms[j].install_progress = -1;
                    g_vms[j].install_status[0] = '\0';
                    post_list_changed();
                    return;
                }
                post_log("[%s] Windows disk built; booting to finish install in the guest.", g_vms[j].name);
                /* Disk is built + bootable, but the guest still has to run its first boot
                   (OOBE + SetupComplete + driver/agent install) — THAT is the "installing"
                   window. So mark disk_built (gates start) but leave install_complete NO; it
                   flips when the guest agent first connects (onOnlineChange), exactly like
                   Windows-on-Windows, which stays "installing" until the agent is up. Progress
                   goes indeterminate (-1) — the first-boot install has no host-side percentage. */
                g_vms[j].disk_built = YES;
                g_vms[j].install_progress = -1;
                strlcpy(g_vms[j].install_status, "Installing Windows", sizeof(g_vms[j].install_status));
                save_vm_list();
                post_event(CORE_VM_EVENT_INSTALL_STATUS, g_vms[j].name, 0, "Installing Windows");
                post_list_changed();
                autostart_after_install(nsName, [VmDir diskImageURLFor:nsName]);
            }];
        });
    });
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

/* Match ECMAScript String.trim exactly; Foundation's built-in whitespace set
   includes NEL and excludes BOM, which would give the API different names. */
static BOOL username_trim_character(unichar ch) {
    return (ch >= 0x0009 && ch <= 0x000D) || ch == 0x0020 || ch == 0x00A0 ||
           ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200A) || ch == 0x2028 ||
           ch == 0x2029 || ch == 0x202F || ch == 0x205F || ch == 0x3000 || ch == 0xFEFF;
}

static NSString *normalized_username(NSString *value) {
    NSUInteger start = 0, end = value.length;
    while (start < end && username_trim_character([value characterAtIndex:start])) start++;
    while (end > start && username_trim_character([value characterAtIndex:end - 1])) end--;
    return [value substringWithRange:NSMakeRange(start, end - start)];
}

NSString *asb_mac_validate_username(NSString *os_type, id username, NSString *vm_name) {
    if (!username) return @"Username is required.";
    if (![username isKindOfClass:[NSString class]])
        return @"Username must be a string.";
    NSString *value = normalized_username(username);
    if (!value.length) return @"Username is required.";
    for (NSUInteger i = 0; i < value.length; i++) {
        unichar ch = [value characterAtIndex:i];
        if (ch == 0) return @"Username cannot contain NUL characters.";
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i >= value.length)
                return @"Username contains invalid Unicode.";
            ch = [value characterAtIndex:i];
            if (ch < 0xDC00 || ch > 0xDFFF)
                return @"Username contains invalid Unicode.";
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return @"Username contains invalid Unicode.";
        }
    }

    BOOL isWindows = os_type && [os_type caseInsensitiveCompare:@"Windows"] == NSOrderedSame;
    if (isWindows) {
        if (value.length > 20) return @"Username cannot exceed 20 characters.";
        BOOL onlyDotsWhitespace = YES;
        for (NSUInteger i = 0; i < value.length; i++) {
            unichar ch = [value characterAtIndex:i];
            if (ch < 32 || ch == 0xFFFE || ch == 0xFFFF ||
                [@"\"\\/[]:;|=,+*?<>%@" rangeOfString:
                    [NSString stringWithCharacters:&ch length:1]].location != NSNotFound)
                return @"Username contains invalid characters.";
            if (ch != '.' && !username_trim_character(ch)) onlyDotsWhitespace = NO;
        }
        if (onlyDotsWhitespace) return @"Username cannot be only dots or spaces.";
        if ([value hasSuffix:@"."]) return @"Username cannot end with a period.";
        NSArray *reserved = @[@"NONE",@"CON",@"PRN",@"AUX",@"NUL",
            @"COM1",@"COM2",@"COM3",@"COM4",@"COM5",@"COM6",@"COM7",@"COM8",@"COM9",
            @"LPT1",@"LPT2",@"LPT3",@"LPT4",@"LPT5",@"LPT6",@"LPT7",@"LPT8",@"LPT9"];
        if ([reserved containsObject:value.uppercaseString])
            return @"Username is a reserved name.";
        if (vm_name && [vm_name caseInsensitiveCompare:value] == NSOrderedSame)
            return @"Username cannot match the VM name (Windows computer name).";
    } else {
        if ([value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 63)
            return @"Username is too long (max 63 UTF-8 bytes in AppSandbox).";
        for (NSUInteger i = 0; i < value.length; i++) {
            unichar ch = [value characterAtIndex:i];
            if (username_trim_character(ch))
                return @"Username cannot contain spaces (macOS short account name).";
        }
        for (NSUInteger i = 0; i < value.length; i++) {
            unichar ch = [value characterAtIndex:i];
            if (ch < 32 || ch == 127 || ch == 0xFFFE || ch == 0xFFFF ||
                ch == '/' || ch == '\\' || ch == ':')
                return @"Username contains invalid characters.";
        }
        if ([value isEqualToString:@"."] || [value isEqualToString:@".."])
            return @"Username cannot be . or .. (macOS short account name).";
        if ([@[@"root",@"daemon",@"nobody",@"guest",@"shared"] containsObject:value.lowercaseString])
            return @"Username is a reserved name.";
    }
    return nil;
}

NSString *asb_mac_validate_password(NSString *os_type, id password) {
    if (!password) return @"Password is required.";
    if (![password isKindOfClass:[NSString class]])
        return @"Password must be a string.";
    NSString *value = password;
    if (!value.length) return @"Password is required.";

    /* Validate before crossing the NUL-terminated UTF-8 API boundary. */
    NSUInteger codePoints = 0;
    for (NSUInteger i = 0; i < value.length; i++) {
        unichar ch = [value characterAtIndex:i];
        if (ch == 0) return @"Password cannot contain NUL characters.";
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i >= value.length)
                return @"Password contains invalid Unicode.";
            ch = [value characterAtIndex:i];
            if (ch < 0xDC00 || ch > 0xDFFF)
                return @"Password contains invalid Unicode.";
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return @"Password contains invalid Unicode.";
        }
        codePoints++;
    }

    if (os_type && [os_type caseInsensitiveCompare:@"Windows"] == NSOrderedSame) {
        if (value.length > 127)
            return @"Password is too long (max 127 characters for Windows).";
    } else {
        if (codePoints < 4)
            return @"Password must be at least 4 characters (macOS minimum).";
        if ([value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 127)
            return @"Password is too long (max 127 UTF-8 bytes in AppSandbox).";
    }
    return nil;
}

int asb_mac_vm_create(const char *name, const char *os_type,
                       int ram_mb, int hdd_gb, int cpu_cores,
                       int gpu_mode, int network_mode,
                       const char *image_path,
                       const char *disk_directory,
                       const char *admin_user,
                       const char *admin_pass,
                       BOOL ssh_enabled,
                       BOOL ssh_deploy_key,
                       BOOL test_mode) {
    if (!name || !os_type) return BACKEND_ERR_INVALID_ARG;
    NSString *username = admin_user ? [NSString stringWithUTF8String:admin_user] : nil;
    NSString *usernameError = (admin_user && !username)
        ? @"Username contains invalid Unicode."
        : asb_mac_validate_username([NSString stringWithUTF8String:os_type], username,
                                   [NSString stringWithUTF8String:name]);
    if (usernameError) {
        post_alert(name, "%s", usernameError.UTF8String);
        return BACKEND_ERR_INVALID_ARG;
    }
    username = normalized_username(username);
    NSString *password = admin_pass ? [NSString stringWithUTF8String:admin_pass] : nil;
    NSString *passwordError = (admin_pass && !password)
        ? @"Password contains invalid Unicode."
        : asb_mac_validate_password([NSString stringWithUTF8String:os_type], password);
    if (passwordError) {
        post_alert(name, "%s", passwordError.UTF8String);
        return BACKEND_ERR_INVALID_ARG;
    }
    if (vm_index_of(name) >= 0) {
        post_alert(name, "A VM named '%s' already exists", name);
        return BACKEND_ERR_INVALID_ARG;
    }
    if (g_vm_count >= ASB_MAX_VMS) {
        post_alert(name, "Maximum number of VMs reached");
        return BACKEND_ERR_FAILED;
    }

    NSString *diskDirectory = disk_directory ? @(disk_directory) : @"";
    NSString *diskError = [VmDir validationErrorForDiskDirectory:diskDirectory vmName:@(name)];
    if (diskError) {
        post_alert(name, "%s", diskError.UTF8String);
        return BACKEND_ERR_INVALID_ARG;
    }
    diskDirectory = [VmDir normalizedDiskDirectory:diskDirectory];
    NSString *imagePath = (image_path && image_path[0])
        ? [NSString stringWithUTF8String:image_path] : nil;

    /* Prompt for admin up front so the user isn't blocked 20 minutes into
     * the install. Token is cached for the process lifetime; subsequent
     * VM creations reuse it silently. Windows-on-Mac has no privileged step
     * (build-windows + QEMU/HVF run unprivileged), so skip the prompt. */
    BOOL isWindows = (os_type && strcasecmp(os_type, "Windows") == 0);
    if (isWindows && imagePath.length == 0) {
        post_alert(name, "A Windows ISO must be selected to create a Windows VM");
        return BACKEND_ERR_INVALID_ARG;
    }
    if (!isWindows) {
        NSError *authErr = nil;
        if (![IsoPatchMac preauthorize:&authErr]) {
            post_alert(name, "Admin authorization required to create VM: %s",
                       authErr.localizedDescription.UTF8String ?: "user cancelled");
            return BACKEND_ERR_FAILED;
        }
    }

    int idx = g_vm_count;
    AsbVmMac *vm = &g_vms[idx];
    memset(vm, 0, sizeof(*vm));
    strlcpy(vm->name, name, sizeof(vm->name));
    strlcpy(vm->os_type, os_type, sizeof(vm->os_type));
    strlcpy(vm->disk_directory, diskDirectory.UTF8String, sizeof(vm->disk_directory));
    vm->ram_mb = ram_mb > 0 ? ram_mb : 8192;
    vm->hdd_gb = hdd_gb > 0 ? hdd_gb : 64;
    vm->cpu_cores = cpu_cores > 0 ? cpu_cores : 4;
    vm->gpu_mode = gpu_mode;
    vm->network_mode = network_mode;
    vm->test_mode = test_mode;   /* honored at start (Windows guest); not forced */
    strlcpy(vm->admin_user, username.UTF8String, sizeof(vm->admin_user));
    strlcpy(vm->admin_pass, admin_pass, sizeof(vm->admin_pass));
    vm->ssh_enabled = ssh_enabled;
    /* Key deploy needs SSH; prepare the AppSandbox keypair now so the instance
       carries the public key (the guest agent deploys it at runtime once the
       guest reports ssh_ready). Mirrors asb_vm_create on Windows. */
    vm->ssh_deploy_key = (ssh_deploy_key && ssh_enabled);
    if (vm->ssh_deploy_key) {
        char pubkey[512];
        if (ensure_appsandbox_ssh_key(pubkey, sizeof(pubkey))) {
            strlcpy(vm->ssh_pubkey, pubkey, sizeof(vm->ssh_pubkey));
        } else {
            post_log("ssh key: could not prepare AppSandbox key; creating without key deploy.");
            vm->ssh_deploy_key = NO;
        }
    }
    vm->install_progress = 0;
    strlcpy(vm->install_status, "Preparing", sizeof(vm->install_status));
    g_vm_count++;
    save_vm_list();

    NSString *nsName = [NSString stringWithUTF8String:name];

    NSError *dirErr = nil;
    if (![VmDir ensureDirectoryFor:nsName error:&dirErr]) {
        post_alert(name, "Failed to create VM directory: %s",
                   dirErr.localizedDescription.UTF8String);
        /* Roll back the slot we appended + persisted (idx == g_vm_count - 1). */
        g_vm_count--;
        memset(&g_vms[idx], 0, sizeof(g_vms[idx]));
        save_vm_list();
        post_list_changed();
        return BACKEND_ERR_FAILED;
    }

    /* ---- Windows guest: from-scratch create from a Microsoft ISO. The user
       picks a .iso; we apply install.wim with our own NTFS writer + stage the
       agent/drivers, then boot via QEMU (always testMode). No IPSW, no DISM. ---- */
    if (vm->os_type[0] && strcasecmp(vm->os_type, "Windows") == 0) {
        run_on_main(^{
            int i = vm_index_of(nsName.UTF8String);
            if (i >= 0) start_windows_build_flow(i, [NSURL fileURLWithPath:imagePath]);
        });
        return BACKEND_OK;
    }

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
                                forVm:nsName
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
    if (!g_vms[idx].disk_built) {
        post_alert(name, "Cannot start: disk is not built yet");
        return BACKEND_ERR_FAILED;
    }

    /* ---- Windows guest: launch via QEMU+HVF + ivshmem (not VZ). testMode (Secure Boot off + test
       signing on) is the create-time choice in g_vms[].test_mode — NOT forced. Our guest drivers are
       test-signed, so a VM created with testMode off won't load them, but the user owns that call. ---- */
    if (vm_is_windows_idx(idx)) {
        if (g_vms[idx].running || g_qemu_refs[idx]) return BACKEND_ERR_ALREADY_RUNNING;
        NSString *nsName = [NSString stringWithUTF8String:name];
        NSURL *vmDir = [VmDir directoryForVm:nsName];
        QemuVm *qvm = [[QemuVm alloc] initWithName:nsName
                                             vmDir:vmDir
                                             ramMb:g_vms[idx].ram_mb
                                          cpuCores:g_vms[idx].cpu_cores
                                          testMode:g_vms[idx].test_mode];
        g_qemu_refs[idx] = qvm;
        qvm.onLog = ^(NSString *l) { post_diag("[%s] qemu: %s", nsName.UTF8String, l.UTF8String); };
        qvm.onStateChange = ^(QemuVmState st) {
            run_on_main(^{ int i = vm_index_of(nsName.UTF8String); if (i >= 0) handle_qemu_state_change(i, st); });
        };
        post_log("[%s] Launching Windows VM via QEMU...", name);
        [qvm startWithCompletion:^(NSError * _Nullable startErr) {
            run_on_main(^{
                int i = vm_index_of(nsName.UTF8String);
                if (i < 0) return;
                if (startErr) {
                    g_qemu_refs[i] = nil;
                    post_log("[%s] Start failed: %s", g_vms[i].name, startErr.localizedDescription.UTF8String);
                    post_alert(g_vms[i].name, "Start failed: %s", startErr.localizedDescription.UTF8String);
                    post_event(CORE_VM_EVENT_STATE_CHANGED, g_vms[i].name, 0, NULL);
                    post_list_changed();
                }
            });
        }];
        return BACKEND_OK;
    }

    /* Idempotent against an in-flight start: a VZ machine exists (vz_handle set)
       from the moment loadVmNamed runs until the Stopped transition, but
       g_vms[].running only flips true on the later Running transition. Guarding
       on BOTH closes the window where an explicit start (client / test) could
       race the post-install auto-start and spin up a SECOND VZ machine on the
       same disk. */
    if (g_vms[idx].running || g_vms[idx].vz_handle) {
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

void asb_mac_vm_set_clipboard_sync(const char *name, BOOL enabled) {
    if (!name) return;
    int idx = vm_index_of(name);
    if (idx < 0) return;
    VmClipboardMac *clip = g_clipboard_refs[idx];
    if (!clip) return;
    [clip setSyncEnabled:enabled];
}

void asb_mac_vm_set_audio_muted(const char *name, BOOL muted) {
    if (!name) return;
    int idx = vm_index_of(name);
    if (idx < 0) return;
    VmAgentMac *agent = g_agent_refs[idx];
    if (!agent || !g_vms[idx].agent_online) return;
    NSString *cmd = muted ? @"mute" : @"unmute";
    /* Fire-and-forget on a background queue — we don't care about the
     * reply, and we don't want to block the main thread of whoever is
     * closing the window. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [agent sendCommand:cmd timeout:3.0];
    });
}

/* ---- Display window (headless daemon, opened on demand) ----
 * The same window the GUI auto-creates on the Running transition, but opened
 * only on an explicit API request. macOS has no separate frame channel/driver
 * (VZVirtualMachineView binds the in-process framebuffer), so there is nothing
 * to probe: readiness is running && agent_online. These run on the main queue
 * (the daemon marshals via on_main); window + VZ live there. */

BOOL asb_mac_have_gui_session(void) {
    CFDictionaryRef info = CGSessionCopyCurrentDictionary();
    if (!info) return NO;   /* no Aqua session at all (launchd system daemon) */
    CFBooleanRef on = CFDictionaryGetValue(info, kCGSessionOnConsoleKey);
    BOOL ok = (on != NULL && CFBooleanGetValue(on));
    CFRelease(info);
    return ok;
}

int asb_mac_open_display(const char *name) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    if (!asb_mac_have_gui_session()) return BACKEND_ERR_NO_DISPLAY;   /* A-gate */
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;
    /* Reap a window the user closed with the X button: its controller lingers
       (g_display_refs holds the only strong ref) until something clears it.
       Doing it here -- on a later request, off the WM close call stack -- is
       safe, unlike clearing it inside windowWillClose:. */
    if (g_display_refs[idx] && ((VzDisplayWindow *)g_display_refs[idx]).userClosed) {
        g_display_refs[idx] = nil;
        g_vms[idx].display  = nil;
    }

    /* Windows guest: the display rides ivshmem ch2 (VDD frames) + ch3 (input), opened/focused as a
       IddDisplayWindow on the VM's transport. Readiness is running && agent_online && idd_ready —
       the VDD only emits ch2 frames once its IDD driver is up (idd_ready, set by the agent's
       onIddStatusChange). The IddDisplayWindow exposes the same window/userClosed/showDisplay
       surface as VzDisplayWindow, so the reap/focus/store sites below it stay uniform. */
    if (vm_is_windows_idx(idx)) {
        if (!g_vms[idx].running)         return BACKEND_ERR_NOT_RUNNING;
        if (!g_vms[idx].agent_online || !g_vms[idx].idd_ready) return BACKEND_ERR_NOT_READY;
        AsbIvshmemTransport *t = g_transport_refs[idx];
        if (!t)                          return BACKEND_ERR_NOT_RUNNING;
        if (g_display_refs[idx]) {   /* already open -> focus */
            [[(IddDisplayWindow *)g_display_refs[idx] window] makeKeyAndOrderFront:nil];
            return BACKEND_OK;
        }
        NSString *nsName = [NSString stringWithUTF8String:name];
        IddDisplayWindow *display = [[IddDisplayWindow alloc] initWithName:nsName transport:t];
        g_display_refs[idx] = display;
        g_vms[idx].display  = (VzDisplayWindow *)display;
        [display showDisplay];
        return BACKEND_OK;
    }

    if (!g_vms[idx].running || !g_vms[idx].vz_handle) return BACKEND_ERR_NOT_RUNNING;
    if (!g_vms[idx].agent_online)                     return BACKEND_ERR_NOT_READY;
    if (g_display_refs[idx]) {   /* already open -> focus */
        [[(VzDisplayWindow *)g_display_refs[idx] window] makeKeyAndOrderFront:nil];
        return BACKEND_OK;
    }
    VzDisplayWindow *display = [[VzDisplayWindow alloc] initWithVzVm:g_vms[idx].vz_handle];
    g_display_refs[idx] = display;
    g_vms[idx].display  = display;
    [display showDisplay];
    return BACKEND_OK;
}

void asb_mac_close_display(const char *name) {
    if (!name) return;
    int idx = vm_index_of(name);
    if (idx < 0 || !g_display_refs[idx]) return;
    VzDisplayWindow *display = g_display_refs[idx];   /* local strong ref keeps it
                                                         alive across the close */
    g_display_refs[idx] = nil;
    g_vms[idx].display  = nil;
    [display.window close];                           /* fires windowWillClose: */
}

int asb_mac_vm_stop(const char *name, int force) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;

    /* Windows guest: stop via QemuVm. Graceful = agent `shutdown` (guest InitiateSystemShutdownExW)
       with an ACPI power-down fallback; force = QMP quit. */
    if (vm_is_windows_idx(idx)) {
        QemuVm *qvm = g_qemu_refs[idx];
        if (!qvm) return BACKEND_ERR_NOT_RUNNING;
        NSString *wName = [NSString stringWithUTF8String:name];
        if (force) { [qvm stop]; return BACKEND_OK; }
        VmAgentMac *wagent = g_agent_refs[idx];
        if (wagent && g_vms[idx].agent_online) {
            post_log("[%s] Requesting graceful shutdown via agent...", name);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                NSString *resp = [wagent sendCommand:@"shutdown" timeout:5.0];
                run_on_main(^{
                    int i = vm_index_of(wName.UTF8String);
                    if (i < 0) return;
                    QemuVm *q = g_qemu_refs[i];
                    if ([resp isEqualToString:@"ok"]) {
                        g_vms[i].shutting_down = YES; post_list_changed();
                    } else if (q) {
                        post_log("[%s] Agent shutdown unresponsive; ACPI power-down.", g_vms[i].name);
                        [q requestStop];
                    }
                });
            });
        } else {
            [qvm requestStop];
        }
        return BACKEND_OK;
    }

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
        return BACKEND_OK;
    }

    /* Prefer the agent path: `shutdown` is handled by our LaunchDaemon,
     * which execs /sbin/shutdown -h now as root. That bypasses the guest's
     * "Are you sure you want to shut down?" dialog that VZ's requestStop
     * triggers (equivalent to a power-button press). Fall back to VZ
     * requestStop only if the agent isn't reachable. */
    VmAgentMac *agent = g_agent_refs[idx];
    if (agent && g_vms[idx].agent_online) {
        post_log("[%s] Requesting graceful shutdown via agent...", name);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSString *resp = [agent sendCommand:@"shutdown" timeout:5.0];
            if ([resp isEqualToString:@"ok"]) {
                /* Flip UI to "shutting down" immediately — the guest's
                 * shutdown -h now takes several seconds before VZ notices
                 * the power-off and emits VZVirtualMachineStateStopping. */
                run_on_main(^{
                    int i = vm_index_of(nsName.UTF8String);
                    if (i < 0) return;
                    g_vms[i].shutting_down = YES;
                    post_list_changed();
                });
                return;
            }
            run_on_main(^{
                int i = vm_index_of(nsName.UTF8String);
                if (i < 0) return;
                VzVm *cur = g_vms[i].vz_handle;
                if (!cur) return;
                post_log("[%s] Agent shutdown unresponsive; using VZ stop.",
                         g_vms[i].name);
                [cur requestStopWithCompletion:onError];
            });
        });
    } else {
        [vm requestStopWithCompletion:onError];
    }

    return BACKEND_OK;
}

int asb_mac_vm_delete(const char *name) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    int idx = vm_index_of(name);
    if (idx < 0) return BACKEND_ERR_NOT_FOUND;

    NSString *nsName = [NSString stringWithUTF8String:name];
    [IsoPatchMac cancelFetchForVm:nsName];

    /* Stop the backend before removing files. A Windows guest runs on QEMU+ivshmem (vz_handle is
       NULL), so its backend must be torn down here — otherwise QEMU keeps running on a disk we are
       about to delete and the ivshmem transport stays mmap'd to a backing file we are about to
       remove, whose async exit callback would crash the daemon. Tear the Windows backend down here,
       synchronously and before VmDir deleteVm, mirroring handle_qemu_state_change's Stopped path. */
    if (vm_is_windows_idx(idx)) {
        if (g_qemu_refs[idx] || g_transport_refs[idx]) {
            post_log("[%s] Stopping Windows VM before delete...", name);
            QemuVm *qvm = g_qemu_refs[idx];
            if (qvm) [qvm stop];                 /* force quit (QMP quit / terminate) */
            stop_agent_for(idx);                 /* stop agent + ssh proxy + clipboard helpers */
            /* Close the display window first: its ch2/ch3 reader threads hold this transport's fds
               (and may call connectChannel on it), so they must be joined BEFORE [t close] unmaps. */
            if (g_display_refs[idx]) {
                IddDisplayWindow *display = g_display_refs[idx];
                [display.window close];          /* fires windowWillClose: -> joins reader threads */
                g_display_refs[idx] = nil;
                g_vms[idx].display = nil;
            }
            AsbIvshmemTransport *t = g_transport_refs[idx];
            if (t) [t close];                    /* munmap + join pumps BEFORE the backing file is removed */
            g_transport_refs[idx] = nil;
            g_qemu_refs[idx] = nil;
        }
        g_vms[idx].running = NO;
        g_vms[idx].shutting_down = NO;
    } else if (g_vms[idx].running && g_vms[idx].vz_handle) {
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
        g_qemu_refs[i] = g_qemu_refs[i + 1];
        g_transport_refs[i] = g_transport_refs[i + 1];
    }
    g_vm_count--;
    memset(&g_vms[g_vm_count], 0, sizeof(AsbVmMac));
    g_vz_refs[g_vm_count] = nil;
    g_display_refs[g_vm_count] = nil;
    g_agent_refs[g_vm_count] = nil;
    g_ssh_proxy_refs[g_vm_count] = nil;
    g_clipboard_refs[g_vm_count] = nil;
    g_qemu_refs[g_vm_count] = nil;
    g_transport_refs[g_vm_count] = nil;

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
        NSURL *oldDiskDir = [VmDir diskDirectoryForVm:oldName];
        NSURL *newDiskDir = [[oldDiskDir URLByDeletingLastPathComponent]
                                URLByAppendingPathComponent:newName isDirectory:YES];
        BOOL external = ![oldDiskDir isEqual:oldDir];
        NSError *err = nil;
        if (external && ![[NSFileManager defaultManager] moveItemAtURL:oldDiskDir toURL:newDiskDir error:&err]) {
            post_alert(name, "Rename disk folder failed: %s", err.localizedDescription.UTF8String);
            return BACKEND_ERR_FAILED;
        }
        if (![[NSFileManager defaultManager] moveItemAtURL:oldDir toURL:newDir error:&err]) {
            if (external)
                [[NSFileManager defaultManager] moveItemAtURL:newDiskDir toURL:oldDiskDir error:nil];
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

void asb_mac_set_headless(BOOL headless) {
    g_headless = headless;
    vz_vm_set_no_audio(headless);
}
