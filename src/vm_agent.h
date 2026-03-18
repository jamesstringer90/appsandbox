#ifndef VM_AGENT_H
#define VM_AGENT_H

#include <windows.h>
#include "hcs_vm.h"

/*
 * Host-side communication with the AppSandbox guest agent.
 * Connects via Hyper-V sockets (AF_HYPERV) using the VM's RuntimeId.
 */

/* Service GUID: {A5B0CAFE-0001-4000-8000-000000000001} */
#define VM_AGENT_SERVICE_GUID_STR L"a5b0cafe-0001-4000-8000-000000000001"

/* Start persistent agent connection for a VM (call after VM starts).
   Spawns a background thread that connects, receives heartbeats,
   and updates instance->agent_online. */
void vm_agent_start(VmInstance *instance);

/* Stop persistent agent connection (call when VM stops/exits). */
void vm_agent_stop(VmInstance *instance);

/* Set the window handle for agent status notifications.
   Posts WM_VM_AGENT_STATUS when agent_online changes. */
void vm_agent_set_hwnd(HWND hwnd);

/* Send a command to the guest agent via the persistent connection.
   Returns TRUE on "ok" reply. Thread-safe. */
BOOL vm_agent_send(VmInstance *instance, const char *command,
                   char *response, int response_max);

BOOL vm_agent_shutdown(VmInstance *instance);
BOOL vm_agent_restart(VmInstance *instance);
BOOL vm_agent_ping(VmInstance *instance);

#endif /* VM_AGENT_H */
