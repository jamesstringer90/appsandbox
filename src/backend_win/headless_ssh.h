#pragma once
#include "headless_vm.h"

typedef struct {
    BYTE *stdout_bytes,*stderr_bytes;
    DWORD stdout_size,stderr_size,exit_code,stdin_error;
    BOOL running;
} AsbSshOutput;
ASB_API DWORD asb_ssh_open(const AsbHeadlessTarget *target,const wchar_t *command,UINT64 *id);
ASB_API DWORD asb_ssh_read(AsbVmId vm_id,UINT64 id,DWORD timeout,AsbSshOutput *output);
ASB_API DWORD asb_ssh_write(AsbVmId vm_id,UINT64 id,const BYTE *bytes,DWORD size,BOOL close_stdin);
ASB_API DWORD asb_ssh_close(AsbVmId vm_id,UINT64 id);
ASB_API void asb_ssh_output_free(AsbSshOutput *output);
ASB_API void asb_ssh_shutdown(AsbVmId vm_id);
