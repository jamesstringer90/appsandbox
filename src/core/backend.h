/*
 * backend.h -- Platform-neutral backend interface.
 *
 * Defines the vtable that each per-platform backend must implement.
 * The C core calls through backend_get() rather than calling any
 * platform API directly, so the same core code can drive HCS on
 * Windows and Virtualization.framework on macOS.
 *
 * All strings are UTF-8 char *. Backends convert to their native
 * encoding at the vtable boundary (UTF-16 on Windows).
 */

#ifndef ASB_CORE_BACKEND_H
#define ASB_CORE_BACKEND_H

#include <stddef.h>
#include "vm_types.h"

#if defined(_WIN32)
  #ifdef ASB_BUILDING_DLL
    #define CORE_API __declspec(dllexport)
  #else
    #define CORE_API __declspec(dllimport)
  #endif
#elif defined(__APPLE__)
  #define CORE_API __attribute__((visibility("default")))
#else
  #define CORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes for vtable functions. 0 = success, anything else = error. */
#define BACKEND_OK                   0
#define BACKEND_ERR_INVALID_ARG     -1
#define BACKEND_ERR_NOT_FOUND       -2
#define BACKEND_ERR_FAILED          -3
#define BACKEND_ERR_NOT_IMPLEMENTED -4
#define BACKEND_ERR_ALREADY_RUNNING -5
#define BACKEND_ERR_NOT_RUNNING     -6

typedef struct BackendVtbl {
    /* Lifecycle. init is called once at process start, cleanup at shutdown. */
    int  (*init)(void);
    void (*cleanup)(void);

    /* VM management. All strings are UTF-8. Non-zero return is an error. */
    int  (*create_vm)(const CoreVmConfig *cfg, char *err, size_t err_sz);
    int  (*start_vm)(const char *name);
    int  (*stop_vm)(const char *name, int force);
    int  (*delete_vm)(const char *name);

    /* Enumerate known VMs. out is caller-allocated. */
    int  (*list_vms)(CoreVmInfo *out, int max_count, int *out_count);

    /* Open a platform-native display window for a running VM.
     * host_window is a platform-specific handle (HWND/NSWindow *) or NULL. */
    int  (*open_display)(const char *name, void *host_window);

    /* Register callback for backend-originated events (state changes,
     * install progress, log lines, alerts). Pass NULL to clear. */
    void (*set_event_cb)(CoreVmEventCallback cb, void *user_data);
} BackendVtbl;

/* Returns a pointer to the active backend's vtable. Never NULL. */
CORE_API const BackendVtbl *backend_get(void);

#ifdef __cplusplus
}
#endif

#endif /* ASB_CORE_BACKEND_H */
