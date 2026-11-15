#include "clipboard_file.h"
#include <wchar.h>
#include <shlobj.h>
#include <string.h>

HGLOBAL clip_file_build_hdrop(const wchar_t *temp_dir)
{
    if (!temp_dir || !*temp_dir) return NULL;
    SIZE_T directory_length = wcslen(temp_dir);
    if (directory_length > MAX_PATH - 3) return NULL;
    WIN32_FIND_DATAW entry;
    wchar_t pattern[MAX_PATH];
    if (swprintf_s(pattern, MAX_PATH, L"%s\\*", temp_dir) < 0) return NULL;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return NULL;
    const SIZE_T limit = 64 * 1024 * 1024;
    SIZE_T capacity = 64, count = 0;
    SIZE_T total = sizeof(DROPFILES) + sizeof(wchar_t);
    wchar_t (*paths)[MAX_PATH] = HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(*paths));
    if (!paths) { FindClose(find); return NULL; }
    BOOL valid = TRUE;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        if (wcslen(entry.cFileName) > MAX_PATH - directory_length - 2) { valid = FALSE; break; }
        if (count == capacity) {
            wchar_t (*grown)[MAX_PATH] = NULL;
            if (capacity <= limit / (2 * sizeof(*paths)))
                grown = HeapReAlloc(GetProcessHeap(), 0, paths, capacity * 2 * sizeof(*paths));
            if (!grown) { valid = FALSE; break; }
            paths = grown;
            capacity *= 2;
        }
        int length = swprintf_s(paths[count], MAX_PATH, L"%s\\%s", temp_dir, entry.cFileName);
        if (length < 0) { valid = FALSE; break; }
        SIZE_T bytes = ((SIZE_T)length + 1) * sizeof(wchar_t);
        if (bytes > limit - total) { valid = FALSE; break; }
        total += bytes;
        count++;
    } while (FindNextFileW(find, &entry));
    if (valid && GetLastError() != ERROR_NO_MORE_FILES) valid = FALSE;
    FindClose(find);
    if (!valid || !count) { HeapFree(GetProcessHeap(), 0, paths); return NULL; }
    HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    DROPFILES *drop = result ? GlobalLock(result) : NULL;
    if (!drop) {
        if (result) GlobalFree(result);
        HeapFree(GetProcessHeap(), 0, paths);
        return NULL;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t *destination = (wchar_t *)((BYTE *)drop + sizeof(DROPFILES));
    for (SIZE_T i = 0; i < count; ++i) {
        SIZE_T length = wcslen(paths[i]) + 1;
        memcpy(destination, paths[i], length * sizeof(wchar_t));
        destination += length;
    }
    *destination = L'\0';
    GlobalUnlock(result);
    HeapFree(GetProcessHeap(), 0, paths);
    return result;
}

void clip_file_writer_abort(ClipFileWriter *file)
{
    DWORD error = GetLastError();
    if (file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
    if (file->buffer) VirtualFree(file->buffer, 0, MEM_RELEASE);
    if (file->path[0]) DeleteFileW(file->path);
    file->handle = INVALID_HANDLE_VALUE;
    file->buffer = NULL;
    file->path[0] = L'\0';
    SetLastError(error);
}

BOOL clip_file_writer_open(ClipFileWriter *file, const wchar_t *path, UINT64 size)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    ZeroMemory(file, sizeof(*file));
    file->handle = INVALID_HANDLE_VALUE;
    if (!path || !path[0] || wcslen(path) >= MAX_PATH || size > MAXLONGLONG) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    file->size = size;
    file->unbuffered = size > 16 * 1024 * 1024;
    if (file->unbuffered) flags |= FILE_FLAG_NO_BUFFERING;
    file->buffer = VirtualAlloc(NULL, CLIP_FILE_BUFFER_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!file->buffer) return FALSE;
    file->handle = CreateFileW(path, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, NULL, CREATE_ALWAYS, flags, NULL);
    if (file->handle == INVALID_HANDLE_VALUE) goto fail;
    wcscpy_s(file->path, MAX_PATH, path);
    if (file->unbuffered) {
        FILE_STORAGE_INFO storage;
        DWORD alignment;
        if (!GetFileInformationByHandleEx(file->handle, FileStorageInfo, &storage, sizeof(storage))) goto fail;
        alignment = storage.LogicalBytesPerSector;
        if (storage.PhysicalBytesPerSectorForPerformance > alignment)
            alignment = storage.PhysicalBytesPerSectorForPerformance;
        if (storage.PhysicalBytesPerSectorForAtomicity > alignment)
            alignment = storage.PhysicalBytesPerSectorForAtomicity;
        if (!alignment || (alignment & (alignment - 1)) || alignment > CLIP_FILE_BUFFER_SIZE ||
            CLIP_FILE_BUFFER_SIZE % alignment || (ULONG_PTR)file->buffer % alignment) {
            SetLastError(ERROR_NOT_SUPPORTED);
            goto fail;
        }
        file->alignment = alignment;
    }
    return TRUE;
fail:
    clip_file_writer_abort(file);
    return FALSE;
}

BOOL clip_file_writer_write(ClipFileWriter *file, DWORD length)
{
    DWORD requested = length;
    DWORD written = 0;
    if (!length || length > CLIP_FILE_BUFFER_SIZE || length > file->size - file->written) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (file->unbuffered && length % file->alignment) {
        if (length != file->size - file->written) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        requested = (length + file->alignment - 1) & ~(file->alignment - 1);
        ZeroMemory(file->buffer + length, requested - length);
    }
    if (!WriteFile(file->handle, file->buffer, requested, &written, NULL)) return FALSE;
    if (written != requested) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    file->written += length;
    return TRUE;
}

BOOL clip_file_writer_finish(ClipFileWriter *file)
{
    DWORD error = ERROR_SUCCESS;
    if (file->written != file->size) error = ERROR_HANDLE_EOF;
    if (!error && file->unbuffered && file->size % file->alignment) {
        FILE_END_OF_FILE_INFO end;
        end.EndOfFile.QuadPart = (LONGLONG)file->size;
        if (!SetFileInformationByHandle(file->handle, FileEndOfFileInfo, &end, sizeof(end))) error = GetLastError();
    }
    if (!CloseHandle(file->handle) && !error) error = GetLastError();
    file->handle = INVALID_HANDLE_VALUE;
    VirtualFree(file->buffer, 0, MEM_RELEASE);
    file->buffer = NULL;
    if (error) DeleteFileW(file->path);
    file->path[0] = L'\0';
    SetLastError(error);
    return error == ERROR_SUCCESS;
}
