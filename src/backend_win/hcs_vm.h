#ifndef HCS_VM_H
#define HCS_VM_H

#include <windows.h>
#include "gpu_enum.h"

/* DLL export/import */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/* Opaque HCS handles */
typedef void *HCS_SYSTEM;
typedef void *HCS_OPERATION;

/* GPU assignment modes */
#define GPU_NONE     0
#define GPU_DEFAULT  1
#define GPU_MIRROR   2

/* Network modes */
#define NET_NONE     0
#define NET_NAT      1
#define NET_EXTERNAL 2
#define NET_INTERNAL 3

/* Configuration for creating a new VM */
typedef struct {
    wchar_t name[256];
    wchar_t os_type[32];          /* L"Windows" or L"Linux" */
    wchar_t image_path[MAX_PATH]; /* ISO path */
    wchar_t vhdx_path[MAX_PATH];  /* will be created if doesn't exist */
    DWORD   ram_mb;
    DWORD   hdd_gb;
    DWORD   cpu_cores;
    int     gpu_mode;             /* GPU_NONE, GPU_DEFAULT, or GPU_MIRROR */
    int     network_mode;         /* NET_NONE, NET_NAT, NET_EXTERNAL, or NET_INTERNAL */
    wchar_t admin_user[128];      /* Guest local admin username */
    wchar_t admin_pass[128];      /* Guest local admin password */
    wchar_t resources_iso_path[MAX_PATH]; /* ISO with autounattend + agent + helpers */
    GpuDriverShareList gpu_shares;         /* Plan9 shares for GPU driver files */
    BOOL    is_template;              /* TRUE = template creation (no GPU/network) */
    BOOL    test_mode;               /* TRUE = disable Secure Boot (for test-signed drivers) */
    BOOL    ssh_enabled;             /* TRUE = install OpenSSH Server in guest */
} VmConfig;

/* Runtime state of a VM */
typedef struct {
    HCS_SYSTEM  handle;
    wchar_t     name[256];
    wchar_t     os_type[32];
    wchar_t     vhdx_path[MAX_PATH];
    wchar_t     image_path[MAX_PATH];
    DWORD       ram_mb;
    DWORD       hdd_gb;
    DWORD       cpu_cores;
    int         gpu_mode;
    wchar_t     gpu_name[256];
    int         network_mode;
    wchar_t     net_adapter[256];     /* Adapter name for External network */
    GUID        network_id;
    GUID        endpoint_id;
    GUID        runtime_id;           /* VM RuntimeId for AF_HYPERV connections */
    wchar_t     resources_iso_path[MAX_PATH];
    BOOL        running;
    BOOL        network_cleaned;  /* TRUE after network teardown (idempotent guard) */
    BOOL        agent_online;     /* TRUE when persistent agent connection is active */
    BOOL        is_template;     /* TRUE = template VM (sysprep after install) */
    BOOL        install_complete; /* TRUE after guest agent first comes online */
    BOOL        test_mode;       /* TRUE = no Secure Boot (for test-signed drivers) */
    BOOL        building_vhdx;   /* TRUE during iso-patch VHDX creation */
    BOOL        vhdx_staging;    /* TRUE during file staging phase */
    int         vhdx_progress;   /* 0-100 progress percentage */
    wchar_t     vhdx_step[128];  /* Current step description */
    GpuDriverShareList gpu_shares; /* Copy of Plan9 share metadata for agent GPU copy */
    void       *hcs_callback;    /* HCS_CALLBACK handle from HcsRegisterComputeSystemCallback */

    /* Monitor thread (safety net for missed HCS callbacks) */
    HANDLE         monitor_thread;
    HANDLE         monitor_stop_event;  /* Signaled to wake monitor thread immediately */
    volatile BOOL  monitor_stop;
    BOOL           callbacks_dead;       /* TRUE if HCS ServiceDisconnect received */
    BOOL           hyperv_video_off;     /* TRUE after guest disables Hyper-V Video adapter */
    volatile BOOL  shutdown_requested;   /* TRUE after graceful shutdown sent */
    ULONGLONG      shutdown_time;        /* GetTickCount64() when shutdown requested */
    volatile ULONGLONG last_heartbeat;   /* GetTickCount64() of last agent heartbeat */
    HANDLE      idd_probe_thread;        /* Background VDD availability probe */
    volatile BOOL idd_probe_stop;
    char        nat_ip[32];              /* Allocated NAT IP (e.g. "172.20.0.2"), empty if not NAT */
    wchar_t     admin_user[128];          /* Guest local admin username */
    BOOL        ssh_enabled;             /* TRUE = OpenSSH Server installed in guest */
    DWORD       ssh_port;                /* Host-side TCP port for SSH tunnel (e.g. 2222) */
    volatile int ssh_state;              /* 0=pending, 1=installing, 2=ready, 3=failed */
} VmInstance;

/* Initialize HCS — loads computecore.dll dynamically.
   Returns TRUE if HCS is available. */
BOOL hcs_init(void);

/* Clean up HCS module. */
void hcs_cleanup(void);

/* Destroy a stale HCS system left over from a previous run. */
void hcs_destroy_stale(const wchar_t *name);

/* Create a VM from configuration. Populates instance on success. */
HRESULT hcs_create_vm(const VmConfig *config, VmInstance *instance);

/* Start a previously created VM. */
HRESULT hcs_start_vm(VmInstance *instance);

/* Graceful shutdown. */
HRESULT hcs_stop_vm(VmInstance *instance);

/* Force terminate. */
HRESULT hcs_terminate_vm(VmInstance *instance);

/* Hot-detach the network adapter from a running VM. */
void hcs_detach_network(VmInstance *instance);

/* Pause a running VM. */
HRESULT hcs_pause_vm(VmInstance *instance);

/* Resume a paused VM. */
HRESULT hcs_resume_vm(VmInstance *instance);

/* Save VM state to a file (memory + device state). */
HRESULT hcs_save_vm(VmInstance *instance, const wchar_t *state_path);

/* Close the HCS handle (does not stop the VM).
   Uses a background thread to avoid blocking the UI. */
ASB_API void hcs_close_vm(VmInstance *instance);

/* Close an HCS handle synchronously (safe to call during atexit). */
void hcs_close_handle_sync(HCS_SYSTEM handle);

/* Try to open an existing HCS compute system by name.
   If found and running, populates instance->handle and returns TRUE. */
BOOL hcs_try_open_vm(VmInstance *instance);

/* Check if a VM is running via HCS enumeration (does not require opening). */
BOOL hcs_is_running_by_enum(const wchar_t *vm_name);

/* Register a callback for VM state changes (e.g. guest-initiated shutdown).
   The callback receives the VmInstance pointer. Called from a worker thread. */
typedef void (*HcsVmStateCallback)(VmInstance *instance, DWORD event);
ASB_API void hcs_set_state_callback(HcsVmStateCallback cb);

/* Register HCS event callback on a VM instance (call after create/open). */
void hcs_register_vm_callback(VmInstance *instance);

/* Unregister HCS event callback (call before closing the HCS handle). */
void hcs_unregister_vm_callback(VmInstance *instance);

/* Set the HWND for monitor thread notifications. */
ASB_API void hcs_set_monitor_hwnd(HWND hwnd);

/* Start the background monitor thread for a running VM. */
void hcs_start_monitor(VmInstance *instance);

/* Stop the background monitor thread. Call before closing HCS handle. */
ASB_API void hcs_stop_monitor(VmInstance *instance);

/* Create a VM with a network endpoint.
   endpoint_guid is the HCN endpoint GUID string (NULL if no network). */
HRESULT hcs_create_vm_with_endpoint(const VmConfig *config, const wchar_t *endpoint_guid,
                                     VmInstance *instance);

/* Build the HCS JSON document for creating a VM.
   json_out must be at least json_out_chars wide chars.
   endpoint_guid is the HCN endpoint GUID string (NULL if no network). */
BOOL hcs_build_vm_json(const VmConfig *config, const wchar_t *endpoint_guid,
                       wchar_t *json_out, size_t json_out_chars);

/* Query guest integration status. Logs GuestConnection info and IC heartbeat.
   Returns TRUE if guest IC services are responding. */
BOOL hcs_query_guest_status(VmInstance *instance);

/* Query HCS properties and return the JSON result.
   Caller must LocalFree the returned string. Returns NULL on failure. */
wchar_t *hcs_query_properties(VmInstance *instance, const wchar_t *query);

/* Find a running VM's RuntimeId by enumerating all HCS compute systems.
   Returns TRUE if found. Does not require a valid HCS_SYSTEM handle. */
BOOL hcs_find_runtime_id(const wchar_t *vm_name, GUID *out);

#endif /* HCS_VM_H */
