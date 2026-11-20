#include "cuda_package.h"
#include <stdio.h>
#include <wchar.h>

static BOOL share_file_name_valid(const wchar_t *name)
{
    if (!wcspbrk(name, L"\\/:") && wcscmp(name, L".") && wcscmp(name, L"..")) return TRUE;
    SetLastError(ERROR_INVALID_NAME);
    return FALSE;
}

BOOL asb_cuda_share_hash(const wchar_t *directory, const wchar_t *files, BYTE digest[ASB_SHA256_SIZE])
{
    wchar_t list[4096], path[MAX_PATH], *context, *name;
    AsbSha256 hash;
    BOOL has_shim = FALSE;
    if (wcscpy_s(list, ARRAYSIZE(list), files) || !asb_sha256_begin(&hash)) return FALSE;
    for (name = wcstok_s(list, L";", &context); name; name = wcstok_s(NULL, L";", &context)) {
        BYTE file_digest[ASB_SHA256_SIZE];
        if (!_wcsicmp(name, ASB_CUDA_SHIM)) has_shim = TRUE;
        if (!share_file_name_valid(name) || swprintf_s(path, MAX_PATH, L"%s\\%s", directory, name) < 0 ||
            !asb_file_sha256(path, file_digest) ||
            !asb_sha256_update(&hash, (const BYTE *)name, (DWORD)((wcslen(name) + 1) * sizeof(*name))) ||
            !asb_sha256_update(&hash, file_digest, sizeof(file_digest))) {
            asb_sha256_close(&hash);
            return FALSE;
        }
    }
    if (!has_shim) {
        asb_sha256_close(&hash);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return asb_sha256_finish(&hash, digest);
}

BOOL asb_cuda_package_id(const wchar_t *driver, const wchar_t *generator,
                         const wchar_t *directory, const wchar_t *files, wchar_t id[65])
{
    static const wchar_t hex[] = L"0123456789abcdef";
    static const BYTE schema[] = "AppSandbox CUDA shim v3";
    BYTE inputs[3][ASB_SHA256_SIZE], digest[ASB_SHA256_SIZE];
    AsbSha256 hash;
    size_t i;
    if (!asb_file_sha256(driver, inputs[0]) || !asb_file_sha256(generator, inputs[1]) ||
        !asb_cuda_share_hash(directory, files, inputs[2]) || !asb_sha256_begin(&hash)) return FALSE;
    if (!asb_sha256_update(&hash, schema, sizeof(schema)) ||
        !asb_sha256_update(&hash, &inputs[0][0], sizeof(inputs))) {
        asb_sha256_close(&hash);
        return FALSE;
    }
    if (!asb_sha256_finish(&hash, digest)) return FALSE;
    for (i = 0; i < sizeof(digest); i++) {
        id[2 * i] = hex[digest[i] >> 4];
        id[2 * i + 1] = hex[digest[i] & 15];
    }
    id[2 * i] = 0;
    return TRUE;
}

BOOL asb_cuda_package_valid(const wchar_t *directory, const wchar_t *files)
{
    wchar_t path[MAX_PATH];
    BYTE expected[ASB_SHA256_SIZE + 1], actual[ASB_SHA256_SIZE];
    HANDLE file;
    DWORD length;
    BOOL ok;
    if (swprintf_s(path, MAX_PATH, L"%s\\%s", directory, ASB_CUDA_CACHE_HASH) < 0) return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = ReadFile(file, expected, sizeof(expected), &length, NULL) && length == sizeof(actual);
    CloseHandle(file);
    if (!ok) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    if (!asb_cuda_share_hash(directory, files, actual)) return FALSE;
    if (memcmp(expected, actual, sizeof(actual))) { SetLastError(ERROR_CRC); return FALSE; }
    return TRUE;
}

BOOL asb_cuda_package_record(const wchar_t *directory, const wchar_t *files)
{
    wchar_t path[MAX_PATH];
    BYTE digest[ASB_SHA256_SIZE];
    HANDLE file;
    DWORD written, error;
    BOOL ok;
    if (swprintf_s(path, MAX_PATH, L"%s\\%s", directory, ASB_CUDA_CACHE_HASH) < 0 ||
        !asb_cuda_share_hash(directory, files, digest)) return FALSE;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, digest, sizeof(digest), &written, NULL) && written == sizeof(digest) &&
        FlushFileBuffers(file);
    error = GetLastError();
    CloseHandle(file);
    if (!ok) SetLastError(error ? error : ERROR_WRITE_FAULT);
    return ok;
}

BOOL asb_cuda_copy_shims(const wchar_t *directory, const wchar_t *target, const wchar_t *files)
{
    wchar_t list[4096], source[MAX_PATH], dest[MAX_PATH], *context, *name;
    if (wcscpy_s(list, ARRAYSIZE(list), files)) return FALSE;
    for (name = wcstok_s(list, L";", &context); name; name = wcstok_s(NULL, L";", &context)) {
        if (!_wcsicmp(name, ASB_CUDA_SHIM)) continue;
        if (!share_file_name_valid(name) ||
            swprintf_s(source, MAX_PATH, L"%s\\%s", directory, name) < 0 ||
            swprintf_s(dest, MAX_PATH, L"%s\\%s", target, name) < 0 ||
            !CopyFileW(source, dest, TRUE)) return FALSE;
    }
    return TRUE;
}

void asb_cuda_remove_package(const wchar_t *directory, const wchar_t *files)
{
    wchar_t list[4096], path[MAX_PATH], *context, *name;
    if (wcscpy_s(list, ARRAYSIZE(list), files)) return;
    for (name = wcstok_s(list, L";", &context); name; name = wcstok_s(NULL, L";", &context)) {
        if (share_file_name_valid(name) && swprintf_s(path, MAX_PATH, L"%s\\%s", directory, name) > 0)
            DeleteFileW(path);
    }
    if (swprintf_s(path, MAX_PATH, L"%s\\%s", directory, ASB_CUDA_CACHE_HASH) > 0)
        DeleteFileW(path);
    RemoveDirectoryW(directory);
}
