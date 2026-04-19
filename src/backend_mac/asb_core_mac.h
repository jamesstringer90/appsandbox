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
    char    admin_user[64];
    char    admin_pass[128];
    int     ram_mb;
    int     hdd_gb;
    int     cpu_cores;
    int     gpu_mode;
    int     network_mode;
    BOOL    running;
    BOOL    shutting_down;
    BOOL    install_complete;
    BOOL    agent_online;
    uint64_t agent_last_heartbeat_ms;
    BOOL    ssh_enabled;            /* user-configured at create time */
    int     ssh_port;               /* host loopback port, 0 = unassigned */
    int     ssh_state;              /* 0=off 1=installing 2=ready 3=failed */
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

int  asb_mac_vm_create(const char *name, const char *os_type,
                        int ram_mb, int hdd_gb, int cpu_cores,
                        int gpu_mode, int network_mode,
                        const char *image_path,
                        const char *admin_user,
                        const char *admin_pass,
                        BOOL ssh_enabled);
int  asb_mac_vm_start(const char *name);
int  asb_mac_vm_stop(const char *name, int force);
int  asb_mac_vm_delete(const char *name);
int  asb_mac_vm_edit(const char *name, const char *field, const char *value);

void asb_mac_save(void);

typedef void (*AsbMacEventCallback)(int type, const char *vm_name,
                                     int int_value, const char *str_value);
void asb_mac_set_event_cb(AsbMacEventCallback cb);

#endif
