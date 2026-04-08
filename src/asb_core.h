/*
 * asb_core.h -- App Sandbox Core Library public API.
 *
 * Provides VM orchestration (create, start, stop, delete), snapshot management,
 * persistence, template scanning, and headless display access.
 *
 * Used by the AppSandbox WebView2 UI and external consumers.
 */

#ifndef ASB_CORE_H
#define ASB_CORE_H

#include <windows.h>

#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif

/* ---- Limits ---- */

#define ASB_MAX_VMS        32
#define ASB_MAX_TEMPLATES  32

/* ---- Opaque handles ---- */

typedef struct AsbVmInternal  AsbVmInternal;
typedef AsbVmInternal        *AsbVm;

/* Forward declare headless display (defined in asb_display.h) */
typedef struct AsbDisplay AsbDisplay;

/* ---- GPU modes ---- */

#define ASB_GPU_NONE     0
#define ASB_GPU_DEFAULT  1

/* ---- Network modes ---- */

#define ASB_NET_NONE     0
#define ASB_NET_NAT      1
#define ASB_NET_EXTERNAL 2
#define ASB_NET_INTERNAL 3

/* ---- VM configuration (for creating a new VM) ---- */

typedef struct {
    const wchar_t *name;
    const wchar_t *os_type;        /* L"Windows" or L"Linux" */
    const wchar_t *image_path;     /* ISO path, or NULL for template */
    const wchar_t *template_name;  /* template name, or NULL for image */
    DWORD  ram_mb;
    DWORD  hdd_gb;
    DWORD  cpu_cores;
    int    gpu_mode;               /* ASB_GPU_NONE or ASB_GPU_DEFAULT */
    int    network_mode;           /* ASB_NET_NONE / NAT / EXTERNAL / INTERNAL */
    const wchar_t *net_adapter;    /* for external mode, or NULL for auto */
    const wchar_t *username;
    const wchar_t *password;
    BOOL   test_mode;              /* TRUE = disable Secure Boot (test-signed drivers) */
    BOOL   is_template;            /* TRUE = create as template VM */
} AsbVmConfig;

/* ---- Snapshot/branch info (returned by query functions) ---- */

typedef struct {
    int      index;
    wchar_t  name[128];
    wchar_t  guid[64];
    int      branch_count;
} AsbSnapshotInfo;

typedef struct {
    int      index;
    wchar_t  name[128];
    wchar_t  guid[64];
} AsbBranchInfo;

/* ---- Callbacks ---- */

/* Log message callback. Called from any thread. */
typedef void (*AsbLogCallback)(const wchar_t *message, void *user_data);

/* VM state change callback. Called when a VM starts, stops, or exits.
   Called from a worker thread — the consumer must marshal to its own thread. */
typedef void (*AsbStateCallback)(AsbVm vm, BOOL running, void *user_data);

/* VHDX creation progress callback.  pct: 0-100, staging: TRUE during file staging phase. */
typedef void (*AsbProgressCallback)(AsbVm vm, int pct, BOOL staging, void *user_data);

/* Alert callback (error messages for the user). */
typedef void (*AsbAlertCallback)(const wchar_t *message, void *user_data);

/* VM removed callback.  Fired when a VM is removed from the internal array
   (e.g. template finalization, failed VHDX creation).  index is the position
   BEFORE compaction — the consumer should compact its parallel arrays.
   Called from a worker thread. */
typedef void (*AsbVmRemovedCallback)(int index, void *user_data);

/* ---- Library init / cleanup ---- */

/* Initialize the core library.
   Loads HCS/HCN DLLs, enumerates GPUs, loads VM list from vms.cfg, scans templates. */
ASB_API HRESULT asb_init(void);

/* Shut down all running VMs and free resources. */
ASB_API void asb_cleanup(void);

/* Cleanly release HCS resources WITHOUT terminating running VMs.
   Call this instead of asb_cleanup() when the process is exiting but
   VMs should keep running (e.g. short-lived processes). */
ASB_API void asb_detach(void);

/* Set callbacks.  All are optional; pass NULL to disable. */
ASB_API void asb_set_log_callback(AsbLogCallback cb, void *user_data);
ASB_API void asb_set_state_callback(AsbStateCallback cb, void *user_data);
ASB_API void asb_set_progress_callback(AsbProgressCallback cb, void *user_data);
ASB_API void asb_set_alert_callback(AsbAlertCallback cb, void *user_data);
ASB_API void asb_set_vm_removed_callback(AsbVmRemovedCallback cb, void *user_data);

/* ---- VM lifecycle ---- */

/* Create a new VM from configuration.
   On success, returns S_OK and the new VM is added to the internal list.
   Depending on config, this may start a background VHDX creation thread. */
ASB_API HRESULT asb_vm_create(const AsbVmConfig *config);

/* Start a VM.
   snap_idx: snapshot index (>= 0), -2 for base, -1 for current disk.
   branch_idx: branch index (>= 0) to resume, or -1 to create a new branch.
   branch_name: friendly name for a newly created branch, or NULL for default. */
ASB_API HRESULT asb_vm_start(AsbVm vm, int snap_idx, int branch_idx,
                              const wchar_t *branch_name);

/* Graceful shutdown (sends ACPI shutdown signal). */
ASB_API HRESULT asb_vm_shutdown(AsbVm vm);

/* Force terminate the VM. */
ASB_API HRESULT asb_vm_stop(AsbVm vm);

/* Stop VM (if running) and delete all files, snapshots, and directories. */
ASB_API HRESULT asb_vm_delete(AsbVm vm);

/* ---- VM queries ---- */

ASB_API int     asb_vm_count(void);
ASB_API AsbVm   asb_vm_get(int index);
ASB_API AsbVm   asb_vm_find(const wchar_t *name);

ASB_API const wchar_t *asb_vm_name(AsbVm vm);
ASB_API const wchar_t *asb_vm_os_type(AsbVm vm);
ASB_API BOOL    asb_vm_is_running(AsbVm vm);
ASB_API BOOL    asb_vm_agent_online(AsbVm vm);
ASB_API BOOL    asb_vm_is_building(AsbVm vm);
ASB_API DWORD   asb_vm_ram_mb(AsbVm vm);
ASB_API DWORD   asb_vm_hdd_gb(AsbVm vm);
ASB_API DWORD   asb_vm_cpu_cores(AsbVm vm);
ASB_API int     asb_vm_gpu_mode(AsbVm vm);
ASB_API int     asb_vm_network_mode(AsbVm vm);

/* ---- VM config editing (VM must be stopped) ---- */

ASB_API HRESULT asb_vm_set_name(AsbVm vm, const wchar_t *name);
ASB_API HRESULT asb_vm_set_ram(AsbVm vm, DWORD ram_mb);
ASB_API HRESULT asb_vm_set_cpu(AsbVm vm, DWORD cores);
ASB_API HRESULT asb_vm_set_gpu(AsbVm vm, int gpu_mode);
ASB_API HRESULT asb_vm_set_network(AsbVm vm, int mode);

/* ---- Snapshots ---- */

ASB_API HRESULT asb_snap_take(AsbVm vm, const wchar_t *name);
ASB_API HRESULT asb_snap_delete(AsbVm vm, int snap_idx);
ASB_API HRESULT asb_snap_new_branch(AsbVm vm, int snap_idx);
ASB_API HRESULT asb_snap_delete_branch(AsbVm vm, int snap_idx, int branch_idx);
ASB_API HRESULT asb_snap_rename(AsbVm vm, int snap_idx, int branch_idx,
                                 const wchar_t *new_name);

ASB_API int     asb_snap_count(AsbVm vm);
ASB_API BOOL    asb_snap_get_info(AsbVm vm, int snap_idx, AsbSnapshotInfo *out);
ASB_API BOOL    asb_snap_get_branch_info(AsbVm vm, int snap_idx, int branch_idx,
                                          AsbBranchInfo *out);

/* Get current snapshot/branch for a VM.
   Sets *snap_idx (-2=base, >=0=snapshot, -1=unknown) and *branch_idx. */
ASB_API void    asb_snap_get_current(AsbVm vm, int *snap_idx, int *branch_idx);

/* ---- Templates ---- */

ASB_API int     asb_template_count(void);
ASB_API const wchar_t *asb_template_name(int index);
ASB_API const wchar_t *asb_template_os_type(int index);
ASB_API HRESULT asb_template_delete(const wchar_t *name);
ASB_API void    asb_template_rescan(void);

/* ---- Wait helpers (blocking) ---- */

/* Block until VM is running (or timeout).  Returns S_OK if running. */
ASB_API HRESULT asb_vm_wait_running(AsbVm vm, DWORD timeout_ms);

/* Block until guest agent comes online (or timeout). */
ASB_API HRESULT asb_vm_wait_agent(AsbVm vm, DWORD timeout_ms);

/* Block until VM has stopped (or timeout). */
ASB_API HRESULT asb_vm_wait_stopped(AsbVm vm, DWORD timeout_ms);

/* ---- Reconnect ---- */

/* Probe HCS for any loaded VMs that are still running and reconnect to them.
   Call after asb_init() in short-lived processes that
   need to see live state of VMs started by a previous process. */
ASB_API void asb_reconnect_running(void);

/* ---- Persistence ---- */

/* Save VM list to vms.cfg.  Normally called automatically; exposed for UI. */
ASB_API void asb_save(void);

/* ---- Settings ---- */

ASB_API void asb_set_last_iso_path(const wchar_t *path);
ASB_API const wchar_t *asb_get_last_iso_path(void);
ASB_API void asb_set_suppress_tray_warn(BOOL suppress);
ASB_API BOOL asb_get_suppress_tray_warn(void);

/* Set HINSTANCE (needed by modules that use ui_get_instance). */
ASB_API void asb_set_hinstance(HINSTANCE hInst);

/* ---- Internal access (for the UI layer) ---- */

/* Get the raw VmInstance pointer (defined in hcs_vm.h).
   Returns NULL if vm handle is invalid.
   The UI needs this to pass VmInstance* to display windows and agent code. */
#ifndef ASB_NO_INTERNAL
#include "hcs_vm.h"
#include "snapshot.h"
#include "gpu_enum.h"

ASB_API VmInstance   *asb_vm_instance(AsbVm vm);
ASB_API SnapshotTree *asb_vm_snap_tree(AsbVm vm);
ASB_API GpuList      *asb_gpu_list(void);
ASB_API int           asb_vm_index(AsbVm vm);
#endif

#endif /* ASB_CORE_H */
