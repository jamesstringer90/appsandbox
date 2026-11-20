#include "cuda_update.h"
#include "../../tools/nvidia/cuda_package.h"
#include <stdio.h>
#include <wchar.h>

#if defined(_M_X64)
static BOOL create_cuda_cache(const wchar_t *directory, wchar_t cache[MAX_PATH])
{
    DWORD attributes;
    if (swprintf_s(cache, MAX_PATH, L"%s\\cuda", directory) < 0) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    if (!CreateDirectoryW(cache, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    attributes = GetFileAttributesW(cache);
    if (attributes == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetLastError(ERROR_DIRECTORY);
        return FALSE;
    }
    return TRUE;
}

static BOOL run_cuda_updater(const wchar_t *updater, const wchar_t *source,
                              const wchar_t *cache, const GpuDriverShare *share)
{
    wchar_t command[MAX_PATH * 4 + ARRAYSIZE(share->file_filter) + 80];
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process;
    DWORD wait, code = 1, error = ERROR_GEN_FAILURE;
    BOOL ok = FALSE;
    if (swprintf_s(command, ARRAYSIZE(command),
                   L"\"%s\" --source \"%s\" --cache \"%s\" --shim-dir \"%s\" --files \"%s\"",
                   updater, source, cache, share->host_path, share->file_filter) < 0) return FALSE;
    if (!CreateProcessW(updater, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                         &startup, &process)) return FALSE;
    CloseHandle(process.hThread);
    wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait == WAIT_OBJECT_0) {
        if (GetExitCodeProcess(process.hProcess, &code)) {
            ok = code == 0;
            error = code;
        } else error = GetLastError();
    } else {
        error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    if (!ok) SetLastError(error);
    return ok;
}
#endif

BOOL cuda_prepare_driver_share(const GpuList *gpus, const wchar_t *gpu_id, GpuDriverShareList *shares)
{
#if defined(_M_X64)
    wchar_t source[MAX_PATH] = {0}, candidate[MAX_PATH], exe[MAX_PATH], *slash;
    wchar_t updater[MAX_PATH], cache[MAX_PATH], output[MAX_PATH], id[65], final_id[65];
    GpuDriverShare *share = NULL;
    DWORD length;
    int i;
    for (i = 0; i < shares->count; i++) {
        if (!_wcsicmp(shares->shares[i].share_name, L"AppSandbox.Nvidia")) {
            share = &shares->shares[i];
            break;
        }
    }
    if (!share) return TRUE;
    for (i = 0; i < gpus->count; i++) {
        const GpuInfo *gpu = &gpus->gpus[i];
        if (gpu_id && *gpu_id && _wcsicmp(gpu_id, gpu->interface_path)) continue;
        if (_wcsicmp(gpu->service, L"nvlddmkm")) continue;
        if (!gpu->driver_store_path[0] || swprintf_s(candidate, MAX_PATH, L"%s\\nvcuda_loader64.dll",
                                                     gpu->driver_store_path) < 0) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
        if (source[0] && !asb_files_sha256_equal(source, candidate)) {
            SetLastError(ERROR_REVISION_MISMATCH);
            return FALSE;
        }
        wcscpy_s(source, MAX_PATH, candidate);
    }
    if (!source[0]) return TRUE;
    length = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!length || length >= MAX_PATH || !(slash = wcsrchr(exe, L'\\'))) return FALSE;
    *slash = 0;
    if (swprintf_s(updater, MAX_PATH, L"%s\\appsandbox-cuda-updater.exe", exe) < 0)
        return FALSE;
    if (!create_cuda_cache(exe, cache) ||
        !asb_cuda_package_id(source, updater, share->host_path, share->file_filter, id) ||
        swprintf_s(output, MAX_PATH, L"%s\\%s", cache, id) < 0 ||
        !run_cuda_updater(updater, source, cache, share) ||
        !asb_cuda_package_id(source, updater, share->host_path, share->file_filter, final_id))
        return FALSE;
    if (wcscmp(id, final_id)) {
        SetLastError(ERROR_RETRY);
        return FALSE;
    }
    if (!asb_cuda_package_valid(output, share->file_filter)) return FALSE;
    wcscpy_s(share->host_path, MAX_PATH, output);
    return TRUE;
#else
    (void)gpus;
    (void)gpu_id;
    (void)shares;
    return TRUE;
#endif
}
