/*
 * asb_types.h -- Platform-neutral VM descriptor types and error codes.
 *
 * Shared by both platform orchestrators (asb_core on Windows,
 * asb_core_mac on macOS) and their UI layers.
 */

#ifndef ASB_TYPES_H
#define ASB_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes shared by both platform orchestrators. */
#define BACKEND_OK                   0
#define BACKEND_ERR_INVALID_ARG     -1
#define BACKEND_ERR_NOT_FOUND       -2
#define BACKEND_ERR_FAILED          -3
#define BACKEND_ERR_NOT_IMPLEMENTED -4
#define BACKEND_ERR_ALREADY_RUNNING -5
#define BACKEND_ERR_NOT_RUNNING     -6

#define CORE_VM_NAME_MAX       128
#define CORE_VM_OS_TYPE_MAX     32
#define CORE_VM_STATUS_MAX     128

/* ---- VM configuration (inputs to create_vm) ---- */

typedef struct CoreVmConfig {
    const char *name;
    const char *os_type;        /* "macOS", "Windows", "Linux" */
    const char *image_path;     /* installer .ipsw/.iso, or NULL */
    const char *template_name;  /* template to clone, or NULL */
    int         ram_mb;
    int         hdd_gb;
    int         cpu_cores;
    int         gpu_mode;       /* 0=none, 1=default */
    int         network_mode;   /* 0=none, 1=NAT, 2=bridged, 3=internal */
    const char *net_adapter;    /* for bridged, or NULL */
    const char *username;
    const char *password;
    int         is_template;
} CoreVmConfig;

/* ---- VM snapshot state (outputs from list_vms) ---- */

typedef struct CoreVmInfo {
    char        name[CORE_VM_NAME_MAX];
    char        os_type[CORE_VM_OS_TYPE_MAX];
    int         running;
    int         shutting_down;
    int         ram_mb;
    int         hdd_gb;
    int         cpu_cores;
    int         gpu_mode;
    char        gpu_name[64];
    int         network_mode;
    int         install_complete;
    int         install_progress;                  /* 0-100, or -1 if not installing */
    char        install_status[CORE_VM_STATUS_MAX];
} CoreVmInfo;

/* ---- Asynchronous events from the backend ---- */

#define CORE_VM_EVENT_STATE_CHANGED   1   /* int_value: 1=running, 0=stopped */
#define CORE_VM_EVENT_PROGRESS        2   /* int_value: 0-100 */
#define CORE_VM_EVENT_LOG             3   /* str_value: log line */
#define CORE_VM_EVENT_ALERT           4   /* str_value: user-visible error */
#define CORE_VM_EVENT_INSTALL_STATUS  5   /* str_value: phase string */
#define CORE_VM_EVENT_LIST_CHANGED    6   /* entire VM list changed */
#define CORE_VM_EVENT_AGENT_STATUS    7   /* int_value: 1=online, 0=offline */
#define CORE_VM_EVENT_DIAG            8   /* protocol/diag — Event Log only */

typedef struct CoreVmEvent {
    int         type;
    char        vm_name[CORE_VM_NAME_MAX];
    int         int_value;
    const char *str_value;  /* may be NULL */
} CoreVmEvent;

typedef void (*CoreVmEventCallback)(const CoreVmEvent *event, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ASB_TYPES_H */
