#ifndef VM_AGENT_H
#define VM_AGENT_H

#include <windows.h>
#include "hcs_vm.h"

/* DLL export/import */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/*
 * Host-side communication with the AppSandbox guest agent.
 * Connects via Hyper-V sockets (AF_HYPERV) using the VM's RuntimeId.
 */

/* Service GUID: {A5B0CAFE-0001-4000-8000-000000000001} */
#define VM_AGENT_SERVICE_GUID_STR L"a5b0cafe-0001-4000-8000-000000000001"

void vm_save_state_json(VmInstance *vm);

/* Start persistent agent connection for a VM (call after VM starts).
   Spawns a background thread that connects, receives heartbeats,
   and updates instance->agent_online. */
void vm_agent_start(VmInstance *instance);

/* Stop persistent agent connection (call when VM stops/exits). */
ASB_API void vm_agent_stop(VmInstance *instance);

/* Set the window handle for agent status notifications.
   Posts WM_VM_AGENT_STATUS when agent_online changes. */
ASB_API void vm_agent_set_hwnd(HWND hwnd);

/* Send a command to the guest agent over the persistent connection. Waits up to
   timeout_ms for the agent's tagged reply and returns TRUE on "ok"; timeout_ms == 0
   is fire-and-forget -- the command is queued for the connection thread and TRUE is
   returned without waiting for any reply (shutdown/restart, which ride the guest
   powering off and have no reliable synchronous reply). Thread-safe. */
ASB_API BOOL vm_agent_send(VmInstance *instance, const char *command,
                           char *response, int response_max, DWORD timeout_ms);

BOOL vm_agent_shutdown(VmInstance *instance);
BOOL vm_agent_restart(VmInstance *instance);
BOOL vm_agent_ping(VmInstance *instance);

#endif /* VM_AGENT_H */
