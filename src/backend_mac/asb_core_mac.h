/*
 * asb_core_mac.h -- macOS orchestrator public API.
 *
 * Mirrors asb_core.h on Windows: owns the in-memory VM array, INI-style
 * persistence (vms.cfg), lifecycle (create/start/stop/delete), and config
 * editing. ui.m calls these functions directly.
 */

#ifndef ASB_CORE_MAC_H
#define ASB_CORE_MAC_H

#import <Foundation/Foundation.h>

@class VzVm, VzDisplayWindow, VmAgentMac, VmSshProxyMac;

#define ASB_MAX_VMS 32

typedef struct {
    char    name[256];
    char    os_type[32];
    char    disk_directory[1024];  /* Optional disk parent; metadata stays in Application Support. */
    char    admin_user[64];
    char    admin_pass[512];       /* UTF-8 storage for up to 127 Windows UTF-16 units. */
    int     ram_mb;
    int     hdd_gb;
    int     cpu_cores;
    int     gpu_mode;
    int     network_mode;
    BOOL    test_mode;             /* Windows guest: Secure Boot off + test-signing on. Set at create,
                                      persisted, read at start. Our guest drivers are test-signed, so a
                                      Windows-on-Mac VM normally needs this — but it is NOT forced. */
    BOOL    running;
    BOOL    shutting_down;
    BOOL    install_complete;      /* "guest provisioned" — the agent has connected at least once
                                      (mirrors Windows install_complete). macOS sets this at build-end
                                      (no first-boot install phase); Windows sets it on first agent-
                                      online, so the first-boot window shows "installing". */
    BOOL    disk_built;            /* disk is built + bootable — gates start (asb_mac_vm_start). Set at
                                      build/stage end for BOTH OSes, distinct from install_complete so a
                                      Windows VM can boot while its first boot is still "installing". */
    BOOL    agent_online;
    BOOL    idd_ready;              /* Windows guest: VDD driver up (from the agent's idd_status);
                                       gates display_ready. macOS guests leave this NO. */
    uint64_t agent_last_heartbeat_ms;
    BOOL    ssh_enabled;            /* user-configured at create time */
    int     ssh_port;               /* host loopback port, 0 = unassigned */
    int     ssh_state;              /* 0=off 1=installing 2=ready 3=failed
                                       (reported as 4 when ready && ssh_key_deployed) */
    BOOL    ssh_deploy_key;         /* TRUE = deploy the AppSandbox public key to the guest */
    BOOL    ssh_key_deployed;       /* TRUE once the guest agent has written authorized_keys
                                       (volatile: reset on stop -- re-deployed each boot) */
    char    ssh_pubkey[512];        /* AppSandbox public-key line to deploy (ed25519) */
    int     install_progress;
    char    install_status[128];
    VzVm            *__unsafe_unretained vz_handle;
    VzDisplayWindow *__unsafe_unretained display;
    VmAgentMac      *__unsafe_unretained agent;
    VmSshProxyMac   *__unsafe_unretained ssh_proxy;
} AsbVmMac;

void asb_mac_init(void);
void asb_mac_cleanup(void);

int          asb_mac_vm_count(void);
AsbVmMac    *asb_mac_vm_get(int index);
AsbVmMac    *asb_mac_vm_find(const char *name);

NSString *asb_mac_validate_username(NSString *os_type, id username, NSString *vm_name);

NSString *asb_mac_validate_password(NSString *os_type, id password);

int  asb_mac_vm_create(const char *name, const char *os_type,
                        int ram_mb, int hdd_gb, int cpu_cores,
                        int gpu_mode, int network_mode,
                        const char *image_path,
                        const char *disk_directory,
                        const char *admin_user,
                        const char *admin_pass,
                        BOOL ssh_enabled,
                        BOOL ssh_deploy_key,
                        BOOL test_mode);
int  asb_mac_vm_start(const char *name);
int  asb_mac_vm_stop(const char *name, int force);
int  asb_mac_vm_delete(const char *name);
int  asb_mac_vm_edit(const char *name, const char *field, const char *value);

void asb_mac_save(void);

/* Send a mute/unmute command to the VM's guest agent. Called from the
 * display window's open/close hooks so the guest stops/starts driving
 * audio when nobody is watching. No-op if the agent isn't online. */
void asb_mac_vm_set_audio_muted(const char *name, BOOL muted);

/* Toggle per-VM clipboard syncing. The display window flips this on
 * becomeKey / resignKey so host clipboard data is only shared with the
 * guest while the user is actually using the VM — avoids background
 * leakage when the user is working in other apps on the host. */
void asb_mac_vm_set_clipboard_sync(const char *name, BOOL enabled);

typedef void (*AsbMacEventCallback)(int type, const char *vm_name,
                                     int int_value, const char *str_value);
void asb_mac_set_event_cb(AsbMacEventCallback cb);

/* Headless mode -- call BEFORE asb_mac_init / any VM start. Gates the parts of
 * the core that need a GUI login session:
 *   - the per-VM NSWindow + VZVirtualMachineView created on the Running
 *     transition (the one window-server dependency in the VM path);
 *   - the clipboard channel (NSPasteboard is per-Aqua-session, and the host
 *     poll/serve machinery must not run in a daemon);
 *   - the VM's audio devices (host microphone capture would hang on a TCC
 *     prompt no daemon can show, and output would play on the host speakers).
 * Display/clipboard teardown paths are nil-guarded no-ops. Mirrors Windows,
 * where the display/clipboard layers live in the GUI app and simply never
 * activate under --headless. */
void asb_mac_set_headless(BOOL headless);

/* ---- Display window (opened on demand by the headless daemon) ----
 * In the GUI the per-VM display NSWindow is created automatically on the Running
 * transition; under --headless it is created only on an explicit request, after
 * two gates. All three run ON THE MAIN QUEUE (the caller marshals) -- the window
 * and the in-process VZVirtualMachine both live there. */

/* A-gate: is there a console (on-console) Aqua login session this process can
 * show a window in? FALSE over SSH / in a launchd service session, where the
 * daemon must refuse to open a display rather than spawn an invisible window. */
BOOL asb_mac_have_gui_session(void);

/* Open (or focus, if already open) the VM's display window. Returns BACKEND_OK,
 * or: BACKEND_ERR_NO_DISPLAY (no GUI session), BACKEND_ERR_NOT_FOUND,
 * BACKEND_ERR_NOT_RUNNING, BACKEND_ERR_NOT_READY (agent not online yet). macOS
 * binds the in-process framebuffer directly (no frame channel / display driver),
 * so readiness is simply running && agent_online -- nothing to probe. */
int  asb_mac_open_display(const char *name);

/* Close the VM's display window if open (no-op otherwise). */
void asb_mac_close_display(const char *name);

/* Path of the AppSandbox SSH private key (~/Library/Application Support/
 * AppSandbox/ssh/id_appsandbox); pair .pub is deployed into guests created
 * with ssh_deploy_key. Returns the path whether or not the key exists yet. */
NSString *asb_mac_ssh_key_path(void);

#endif
