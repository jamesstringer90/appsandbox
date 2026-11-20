#include "gpu_delivery.h"
#include "asb_core.h"
#include <wchar.h>

#define GPU_DELIVERY_VERSION 3

static BOOL delivery_path(const wchar_t *disk, wchar_t path[MAX_PATH])
{
    wchar_t directory[MAX_PATH];
    return get_vm_disk_root(disk, directory) &&
        swprintf_s(path, MAX_PATH, L"%s\\gpu-delivery.dat", directory) > 0;
}

static BOOL invalidate_delivery(const wchar_t *path)
{
    return DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

void gpu_forget_delivery(const wchar_t *disk)
{
    wchar_t path[MAX_PATH];
    if (delivery_path(disk, path)) invalidate_delivery(path);
}

static BOOL identify_disk(const wchar_t *disk, GpuShimDeliveryRecord *record)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE file = CreateFileW(disk, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    if (!ok || !(info.nFileIndexHigh || info.nFileIndexLow)) return FALSE;
    record->volume = info.dwVolumeSerialNumber;
    record->file_high = info.nFileIndexHigh;
    record->file_low = info.nFileIndexLow;
    record->created = info.ftCreationTime;
    return TRUE;
}

static BOOL read_delivery(const wchar_t *path, const GpuShimDeliveryRecord *current,
                          GpuShimDeliveryRecord *previous)
{
    BYTE bytes[sizeof(*previous) + 1];
    DWORD size;
    BOOL ok;
    HANDLE file = CreateFileW(path, GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = ReadFile(file, bytes, sizeof(bytes), &size, NULL) && size == sizeof(*previous);
    CloseHandle(file);
    if (!ok) return FALSE;
    memcpy(previous, bytes, sizeof(*previous));
    return previous->version == GPU_DELIVERY_VERSION && previous->count &&
        previous->count <= ARRAYSIZE(previous->files) && previous->vm_id == current->vm_id &&
        previous->volume == current->volume && previous->file_high == current->file_high &&
        previous->file_low == current->file_low &&
        !CompareFileTime(&previous->created, &current->created) &&
        !_wcsnicmp(previous->guest_path, current->guest_path, MAX_PATH);
}

static BOOL hash_shim(const wchar_t *folder, const wchar_t *name, BYTE sha256[ASB_SHA256_SIZE])
{
    wchar_t path[MAX_PATH];
    return swprintf_s(path, MAX_PATH, L"%s\\%s", folder, name) > 0 && asb_file_sha256(path, sha256);
}

static BOOL shim_delivered(const GpuShimDeliveryRecord *previous, const GpuShimFile *file)
{
    DWORD i;
    for (i = 0; i < previous->count; i++) {
        if (!_wcsnicmp(previous->files[i].name, file->name, ARRAYSIZE(file->name)) &&
            !memcmp(previous->files[i].sha256, file->sha256, sizeof(file->sha256)))
            return TRUE;
    }
    return FALSE;
}

const wchar_t *gpu_shim_delivery_filter(UINT64 vm_id, const wchar_t *disk, const GpuDriverShare *share,
                                       wchar_t filter[4096], GpuShimDelivery *pending)
{
    GpuShimDelivery next = {0};
    GpuShimDeliveryRecord previous;
    wchar_t list[ARRAYSIZE(share->file_filter)], *context, *name;

    if (!delivery_path(disk, next.path)) return share->file_filter;
    if (!share->file_filter[0] || !identify_disk(disk, &next.record)) goto fallback;
    next.record.version = GPU_DELIVERY_VERSION;
    next.record.vm_id = vm_id;
    if (wcscpy_s(next.record.guest_path, MAX_PATH, share->guest_path) ||
        wcscpy_s(next.source, MAX_PATH, share->host_path) ||
        wcscpy_s(list, ARRAYSIZE(list), share->file_filter))
        goto fallback;
    if (!read_delivery(next.path, &next.record, &previous)) previous.count = 0;

    filter[0] = 0;
    for (name = wcstok_s(list, L";", &context); name; name = wcstok_s(NULL, L";", &context)) {
        GpuShimFile *file;
        if (next.record.count == ARRAYSIZE(next.record.files)) goto fallback;
        file = &next.record.files[next.record.count++];
        if (wcscpy_s(file->name, ARRAYSIZE(file->name), name) ||
            !hash_shim(next.source, file->name, file->sha256)) goto fallback;
        if (shim_delivered(&previous, file)) continue;
        if ((filter[0] && wcscat_s(filter, 4096, L";")) || wcscat_s(filter, 4096, name))
            goto fallback;
        next.requested++;
    }
    if (!next.record.count) goto fallback;
    if (!next.requested) return L";";
    if (!invalidate_delivery(next.path)) return share->file_filter;
    *pending = next;
    return filter;

fallback:
    invalidate_delivery(next.path);
    return share->file_filter;
}

void gpu_confirm_shim_delivery(const GpuShimDelivery *pending, DWORD copied)
{
    GpuShimDelivery delivery = *pending;
    BYTE sha256[ASB_SHA256_SIZE];
    DWORD i, size;
    HANDLE file;
    BOOL ok;

    if (!delivery.requested || copied < delivery.requested) return;
    for (i = 0; i < delivery.record.count; i++) {
        const GpuShimFile *shim = &delivery.record.files[i];
        if (!hash_shim(delivery.source, shim->name, sha256) ||
            memcmp(sha256, shim->sha256, sizeof(sha256)))
            return;
    }
    file = CreateFileW(delivery.path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    ok = WriteFile(file, &delivery.record, sizeof(delivery.record), &size, NULL) &&
         size == sizeof(delivery.record) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) invalidate_delivery(delivery.path);
}
