#ifndef ASB_CUDA_UPDATE_H
#define ASB_CUDA_UPDATE_H

#include "gpu_enum.h"

BOOL cuda_prepare_driver_share(const GpuList *gpus, const wchar_t *gpu_id, GpuDriverShareList *shares);

#endif
