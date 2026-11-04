#ifndef ASB_GPU_BROKER_H
#define ASB_GPU_BROKER_H

#include <windows.h>

typedef struct AsbGpuBroker AsbGpuBroker;

BOOL asb_gpu_broker_dispatch(int *result);
AsbGpuBroker *asb_gpu_broker_open(const GUID *vm, LUID adapter);
HRESULT asb_gpu_broker_import(AsbGpuBroker *broker, UINT64 handle, BOOL fence,
                             UINT width, UINT height, HANDLE *imported);
void asb_gpu_broker_close(AsbGpuBroker *broker);

#endif
