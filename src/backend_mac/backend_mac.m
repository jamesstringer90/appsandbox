/*
 * backend_mac.m -- macOS implementation of the core BackendVtbl.
 *
 * Thin adapter layer over the Objective-C VZ modules (vz_vm, vz_install,
 * vz_display, vz_disk, vz_network, vm_dir, host_info). Responsibilities:
 *
 *  - Dispatch VZ calls to the main queue (VZ requires main thread).
 *  - Track in-flight installs so list_vms can surface progress to the UI.
 *  - Translate CoreVm* structs to/from the on-disk VM directory layout.
 *  - Post VM_EVENT_* callbacks for state changes, progress, logs, alerts.
 *
 * All public entry points are thread-safe: state dictionaries are only
 * mutated on the main queue, and callers from any thread go through a
 * main-queue dispatch before touching g_vms or g_installing.
 */

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Virtualization/Virtualization.h>

#include "backend.h"
#include "vz_vm.h"
#include "vz_install.h"
#include "vz_display.h"
#include "vz_disk.h"
#include "vz_network.h"
#include "vm_dir.h"
#include "host_info.h"

/* ---- Global state (main-queue only) ---- */

static NSMutableDictionary<NSString *, VzVm *> *g_vms;            /* running VMs */
static NSMutableDictionary<NSString *, VzDisplayWindow *> *g_displays;
static NSMutableDictionary<NSString *, NSDictionary *> *g_installing; /* name -> {progress, stage} */
static CoreVmEventCallback g_event_cb;
static void *g_event_user_data;

/* ---- Helpers ---- */

static void run_on_main(dispatch_block_t block) {
    if ([NSThread isMainThread]) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

static void post_event(int type, NSString *vmName, int intValue, NSString *strValue) {
    if (!g_event_cb) return;
    CoreVmEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.int_value = intValue;
    if (vmName) {
        strlcpy(evt.vm_name, [vmName UTF8String], sizeof(evt.vm_name));
    }
    const char *s = strValue ? [strValue UTF8String] : NULL;
    evt.str_value = s;
    g_event_cb(&evt, g_event_user_data);
}

static void post_log(NSString *msg) {
    post_event(CORE_VM_EVENT_LOG, nil, 0, msg);
}

static void post_alert(NSString *vmName, NSString *msg) {
    post_event(CORE_VM_EVENT_ALERT, vmName, 0, msg);
}

static void post_list_changed(void) {
    post_event(CORE_VM_EVENT_LIST_CHANGED, nil, 0, nil);
}

static void post_state_changed(NSString *name, BOOL running) {
    post_event(CORE_VM_EVENT_STATE_CHANGED, name, running ? 1 : 0, nil);
}

static void update_install_progress(NSString *name, double frac, NSString *stage) {
    run_on_main(^{
        int pct = (int)(frac * 100.0);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        NSMutableDictionary *entry = [NSMutableDictionary dictionary];
        entry[@"progress"] = @(pct);
        if (stage) entry[@"stage"] = stage;
        g_installing[name] = entry;
        post_event(CORE_VM_EVENT_PROGRESS, name, pct, nil);
        if (stage) post_event(CORE_VM_EVENT_INSTALL_STATUS, name, pct, stage);
        post_list_changed();
    });
}

static void finish_install(NSString *name, NSError *error) {
    run_on_main(^{
        [g_installing removeObjectForKey:name];
        if (error) {
            post_alert(name, [NSString stringWithFormat:@"Install failed: %@", error.localizedDescription]);
            /* Leave the directory behind so the user can inspect logs; they can
             * delete it from the UI. */
        } else {
            post_event(CORE_VM_EVENT_INSTALL_STATUS, name, 100, @"Install complete");
            post_event(CORE_VM_EVENT_PROGRESS, name, 100, nil);
        }
        post_list_changed();
    });
}

/* ---- Lifecycle ---- */

static int backend_mac_init(void) {
    run_on_main(^{
        if (!g_vms)        g_vms = [NSMutableDictionary dictionary];
        if (!g_displays)   g_displays = [NSMutableDictionary dictionary];
        if (!g_installing) g_installing = [NSMutableDictionary dictionary];
    });
    return BACKEND_OK;
}

static void backend_mac_cleanup(void) {
    run_on_main(^{
        g_vms = nil;
        g_displays = nil;
        g_installing = nil;
        g_event_cb = NULL;
        g_event_user_data = NULL;
    });
}

/* ---- Create VM (install) ---- */

static void start_install_flow(NSString *name, NSURL *restoreURL,
                               int ramMb, int hddGb, int cpus) {
    post_event(CORE_VM_EVENT_INSTALL_STATUS, name, 0, @"Starting install");
    post_list_changed();
    [VzInstall installMacOSWithName:name
                    restoreImageURL:restoreURL
                              ramMb:ramMb
                              hddGb:hddGb
                           cpuCores:cpus
                           progress:^(double frac, NSString *stage) {
        update_install_progress(name, frac, stage);
    }
                         completion:^(NSError * _Nullable err) {
        finish_install(name, err);
    }];
}

static int backend_mac_create_vm(const CoreVmConfig *cfg, char *err, size_t err_sz) {
    if (!cfg || !cfg->name || !cfg->os_type) {
        if (err && err_sz) strlcpy(err, "missing name/os_type", err_sz);
        return BACKEND_ERR_INVALID_ARG;
    }

    NSString *os = [NSString stringWithUTF8String:cfg->os_type];
    if (![os isEqualToString:@"macOS"]) {
        if (err && err_sz) strlcpy(err, "macOS backend only supports macOS guests", err_sz);
        return BACKEND_ERR_NOT_IMPLEMENTED;
    }
    if (cfg->is_template) {
        if (err && err_sz) strlcpy(err, "templates are not yet supported on macOS", err_sz);
        return BACKEND_ERR_NOT_IMPLEMENTED;
    }

    NSString *name = [NSString stringWithUTF8String:cfg->name];
    NSString *imagePath = (cfg->image_path && cfg->image_path[0])
        ? [NSString stringWithUTF8String:cfg->image_path] : nil;
    int ramMb = cfg->ram_mb > 0 ? cfg->ram_mb : 8192;
    int hddGb = cfg->hdd_gb > 0 ? cfg->hdd_gb : 64;
    int cpus  = cfg->cpu_cores > 0 ? cfg->cpu_cores : 4;

    run_on_main(^{
        if ([VmDir vmExists:name]) {
            post_alert(name, [NSString stringWithFormat:@"A VM named '%@' already exists", name]);
            return;
        }
        NSError *dirErr = nil;
        if (![VmDir ensureDirectoryFor:name error:&dirErr]) {
            post_alert(name, [NSString stringWithFormat:@"Failed to create VM directory: %@",
                              dirErr.localizedDescription]);
            return;
        }
        /* Seed config so list_vms shows ram/cpu/hdd immediately. */
        [VmDir writeConfig:@{
            @"ramMb":    @(ramMb),
            @"cpuCores": @(cpus),
            @"hddGb":    @(hddGb),
            @"osType":   @"macOS",
        } for:name error:nil];

        g_installing[name] = @{ @"progress": @(0), @"stage": @"Preparing" };
        post_list_changed();

        if (imagePath.length > 0) {
            start_install_flow(name, [NSURL fileURLWithPath:imagePath], ramMb, hddGb, cpus);
            return;
        }

        post_event(CORE_VM_EVENT_INSTALL_STATUS, name, 0, @"Fetching latest restore image");
        [VzInstall fetchLatestRestoreImageURLWithCompletion:^(NSURL * _Nullable url, NSError * _Nullable fetchErr) {
            if (!url) {
                finish_install(name, fetchErr ?: [NSError errorWithDomain:@"VzInstall" code:20
                    userInfo:@{NSLocalizedDescriptionKey: @"No supported restore image available"}]);
                return;
            }
            NSURL *localIpsw = [[VmDir directoryForVm:name] URLByAppendingPathComponent:@"restore.ipsw"];
            [VzInstall downloadRestoreImageFromURL:url
                                             toURL:localIpsw
                                          progress:^(double frac, NSString *stage) {
                update_install_progress(name, frac, stage);
            }
                                        completion:^(NSError * _Nullable dlErr) {
                if (dlErr) {
                    finish_install(name, dlErr);
                    return;
                }
                start_install_flow(name, localIpsw, ramMb, hddGb, cpus);
            }];
        }];
    });

    return BACKEND_OK;
}

/* ---- Start / stop / delete ---- */

static int backend_mac_start_vm(const char *name) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    NSString *n = [NSString stringWithUTF8String:name];

    if (![VzInstall isInstallCompleteFor:n]) {
        post_alert(n, @"Cannot start: install has not completed");
        return BACKEND_ERR_FAILED;
    }

    run_on_main(^{
        if (g_vms[n]) {
            post_alert(n, @"VM is already running");
            return;
        }
        NSError *err = nil;
        VzVm *vm = [VzVm loadVmNamed:n error:&err];
        if (!vm) {
            post_alert(n, [NSString stringWithFormat:@"Load failed: %@",
                           err ? err.localizedDescription : @"unknown"]);
            return;
        }
        g_vms[n] = vm;
        [vm startWithCompletion:^(NSError * _Nullable startErr) {
            run_on_main(^{
                if (startErr) {
                    [g_vms removeObjectForKey:n];
                    post_alert(n, [NSString stringWithFormat:@"Start failed: %@",
                                   startErr.localizedDescription]);
                    post_state_changed(n, NO);
                    post_list_changed();
                } else {
                    post_state_changed(n, YES);
                    post_list_changed();
                }
            });
        }];
    });
    return BACKEND_OK;
}

static int backend_mac_stop_vm(const char *name, int force) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    NSString *n = [NSString stringWithUTF8String:name];

    __block int rc = BACKEND_OK;
    run_on_main(^{
        VzVm *vm = g_vms[n];
        if (!vm) { rc = BACKEND_ERR_NOT_RUNNING; return; }

        void (^onDone)(NSError *) = ^(NSError * _Nullable stopErr) {
            run_on_main(^{
                if (stopErr) {
                    post_alert(n, [NSString stringWithFormat:@"Stop failed: %@",
                                   stopErr.localizedDescription]);
                    return;
                }
                [g_displays removeObjectForKey:n];
                [g_vms removeObjectForKey:n];
                post_state_changed(n, NO);
                post_list_changed();
            });
        };

        if (force) {
            [vm stopWithCompletion:onDone];
        } else {
            [vm requestStopWithCompletion:onDone];
        }
    });
    return rc;
}

static int backend_mac_delete_vm(const char *name) {
    if (!name) return BACKEND_ERR_INVALID_ARG;
    NSString *n = [NSString stringWithUTF8String:name];

    __block int rc = BACKEND_OK;
    run_on_main(^{
        if (g_vms[n]) { rc = BACKEND_ERR_ALREADY_RUNNING; return; }
        [g_installing removeObjectForKey:n];
        NSError *err = nil;
        if (![VmDir deleteVm:n error:&err]) {
            post_alert(n, [NSString stringWithFormat:@"Delete failed: %@", err.localizedDescription]);
            rc = BACKEND_ERR_FAILED;
            return;
        }
        post_list_changed();
    });
    return rc;
}

/* ---- list_vms ---- */

static int backend_mac_list_vms(CoreVmInfo *out, int max_count, int *out_count) {
    if (!out || !out_count) return BACKEND_ERR_INVALID_ARG;

    __block int count = 0;
    dispatch_block_t work = ^{
        NSArray<NSString *> *names = [VmDir listAllVmNames];
        for (NSString *name in names) {
            if (count >= max_count) break;
            CoreVmInfo *info = &out[count];
            memset(info, 0, sizeof(*info));
            strlcpy(info->name, [name UTF8String], sizeof(info->name));
            strlcpy(info->os_type, "macOS", sizeof(info->os_type));

            info->running = g_vms[name] != nil ? 1 : 0;

            NSDictionary *cfgDict = [VmDir readConfigFor:name];
            info->ram_mb = [cfgDict[@"ramMb"] intValue];
            info->cpu_cores = [cfgDict[@"cpuCores"] intValue];
            info->hdd_gb = [cfgDict[@"hddGb"] intValue];

            NSDictionary *inst = g_installing[name];
            if (inst) {
                /* Installing — show as "running" so the UI paints the spinner. */
                info->running = 1;
                info->install_complete = 0;
                info->install_progress = [inst[@"progress"] intValue];
                NSString *stage = inst[@"stage"];
                if (stage) strlcpy(info->install_status, [stage UTF8String], sizeof(info->install_status));
            } else if ([VzInstall isInstallCompleteFor:name]) {
                info->install_complete = 1;
                info->install_progress = 100;
            } else {
                info->install_complete = 0;
                info->install_progress = -1;
            }
            count++;
        }
    };

    if ([NSThread isMainThread]) {
        work();
    } else {
        dispatch_sync(dispatch_get_main_queue(), work);
    }

    *out_count = count;
    return BACKEND_OK;
}

/* ---- Display ---- */

static int backend_mac_open_display(const char *name, void *host_window) {
    (void)host_window;
    if (!name) return BACKEND_ERR_INVALID_ARG;
    NSString *n = [NSString stringWithUTF8String:name];

    __block int rc = BACKEND_OK;
    run_on_main(^{
        VzVm *vm = g_vms[n];
        if (!vm) { rc = BACKEND_ERR_NOT_RUNNING; return; }

        VzDisplayWindow *display = g_displays[n];
        if (!display) {
            display = [[VzDisplayWindow alloc] initWithVzVm:vm];
            g_displays[n] = display;
        }
        [display showDisplay];
    });
    return rc;
}

static void backend_mac_set_event_cb(CoreVmEventCallback cb, void *user_data) {
    g_event_cb = cb;
    g_event_user_data = user_data;
}

/* ---- Vtable ---- */

static const BackendVtbl g_backend_mac = {
    backend_mac_init,
    backend_mac_cleanup,
    backend_mac_create_vm,
    backend_mac_start_vm,
    backend_mac_stop_vm,
    backend_mac_delete_vm,
    backend_mac_list_vms,
    backend_mac_open_display,
    backend_mac_set_event_cb,
};

const BackendVtbl *backend_get(void) {
    return &g_backend_mac;
}
