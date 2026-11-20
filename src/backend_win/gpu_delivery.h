#ifndef ASB_GPU_DELIVERY_H
#define ASB_GPU_DELIVERY_H

#include "gpu_enum.h"
#include "../core/win_file_hash.h"

typedef struct {
    wchar_t name[64];
    BYTE sha256[ASB_SHA256_SIZE];
} GpuShimFile;

typedef struct {
    DWORD version;
    DWORD count;
    UINT64 vm_id;
    DWORD volume;
    DWORD file_high;
    DWORD file_low;
    FILETIME created;
    wchar_t guest_path[MAX_PATH];
    GpuShimFile files[5];
} GpuShimDeliveryRecord;

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t source[MAX_PATH];
    DWORD requested;
    GpuShimDeliveryRecord record;
} GpuShimDelivery;

const wchar_t *gpu_shim_delivery_filter(UINT64 vm_id, const wchar_t *disk, const GpuDriverShare *share,
                                       wchar_t filter[4096], GpuShimDelivery *pending);
void gpu_confirm_shim_delivery(const GpuShimDelivery *pending, DWORD copied);
void gpu_forget_delivery(const wchar_t *disk);

#endif
