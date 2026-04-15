#ifndef VM_SSH_PROXY_H
#define VM_SSH_PROXY_H

#include "hcs_vm.h"

/* SSH proxy service GUID: {A5B0CAFE-0007-4000-8000-000000000001} */
#define VM_SSH_SERVICE_GUID_STR L"a5b0cafe-0007-4000-8000-000000000001"

/* Start the host-side TCP-to-HV-socket SSH proxy for a VM.
   Binds an ephemeral port on 127.0.0.1, relays to guest AF_HYPERV :0007.
   Updates instance->ssh_port with the actual bound port.
   No-op if ssh_enabled is FALSE or proxy is already running. */
void vm_ssh_proxy_start(VmInstance *instance);

/* Stop the SSH proxy for a VM.  Safe to call even if not started. */
void vm_ssh_proxy_stop(VmInstance *instance);

#endif /* VM_SSH_PROXY_H */
