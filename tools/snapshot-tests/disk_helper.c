/* asb-disk-helper: attaches test VHDX files and reads or writes sectors through them.

   It runs elevated, so that the snapshot code and the tests never have to. It writes
   to \\.\PhysicalDriveN, so writing to the wrong N would destroy a real disk. Hence:

   - It only touches .vhdx files inside the folder named on its command line, checked
     on the final path, so a junction cannot walk out of that folder.
   - It only attaches disks of exactly HELPER_DISK_SIZE.
   - Every drive that exists when it starts is off limits for good.
   - Before opening a drive for writing, guard_check() proves, through a read-only
     handle, that the drive is the file-backed disk this request just attached.

   Attaches are never permanent: they end with the handle, the process, or the power.
   It exits after HELPER_IDLE_MS without a request. */
#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <sddl.h>
#include <stdio.h>
#include <wchar.h>
#include "disk_protocol.h"

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "advapi32.lib")

#define HELPER_IDLE_MS (12ULL * 60 * 60 * 1000)
#define MAX_DRIVES 64

static const GUID VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

static wchar_t g_root[MAX_PATH];
static BOOL g_real_drive[MAX_DRIVES];
static HANDLE g_held = INVALID_HANDLE_VALUE;
static volatile ULONGLONG g_last_request;
static FILE *g_log;

static DWORD fail(HelperReply *rep, DWORD stage, DWORD error, const wchar_t *why)
{
    rep->stage = stage;
    rep->error = error;
    swprintf_s(rep->text, HELPER_MAX_PATH, L"%s (error %lu)", why, error);
    return error;
}

static BOOL final_path(const wchar_t *in, DWORD flags, wchar_t *out, size_t out_len)
{
    wchar_t buf[HELPER_MAX_PATH + 8];
    const wchar_t *p = buf;
    HANDLE h = CreateFileW(in, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, flags, NULL);
    DWORD n;

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    n = GetFinalPathNameByHandleW(h, buf, HELPER_MAX_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= HELPER_MAX_PATH) return FALSE;
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    return wcscpy_s(out, out_len, p) == 0;
}

/* The fence: a .vhdx file, whose real location is inside the test folder. */
static DWORD resolve_disk(const wchar_t *in, wchar_t *out, size_t out_len, HelperReply *rep)
{
    size_t root_len = wcslen(g_root);
    size_t len;

    if (wcsnlen(in, HELPER_MAX_PATH) >= HELPER_MAX_PATH)
        return fail(rep, STAGE_REQUEST, ERROR_BAD_PATHNAME, L"path not terminated");
    if (!final_path(in, FILE_ATTRIBUTE_NORMAL, out, out_len))
        return fail(rep, STAGE_PATH, GetLastError() ? GetLastError() : ERROR_BAD_PATHNAME, L"cannot resolve path");
    len = wcslen(out);
    if (len <= root_len + 1 || _wcsnicmp(out, g_root, root_len) != 0 || out[root_len] != L'\\')
        return fail(rep, STAGE_PATH, ERROR_ACCESS_DENIED, L"outside the test folder");
    if (len < 5 || _wcsicmp(out + len - 5, L".vhdx") != 0)
        return fail(rep, STAGE_PATH, ERROR_ACCESS_DENIED, L"not a .vhdx file");
    return ERROR_SUCCESS;
}

/* Proves that `physical` is the test disk backed by `disk`, through a read-only handle.
   OP_GUARD_PROBE passes `skip` so each check can be seen to refuse on its own; every
   path that can write passes 0. The backing check cannot be skipped. */
static DWORD guard_check(const wchar_t *physical, const wchar_t *disk, DWORD skip, HelperReply *rep)
{
    static const wchar_t prefix[] = L"\\\\.\\PhysicalDrive";
    const size_t prefix_len = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    STORAGE_DEVICE_NUMBER number;
    STORAGE_PROPERTY_QUERY query;
    GET_LENGTH_INFORMATION length;
    BYTE desc_buf[1024];
    STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)desc_buf;
    static BYTE dep_buf[16384];
    STORAGE_DEPENDENCY_INFO *dep = (STORAGE_DEPENDENCY_INFO *)dep_buf;
    const wchar_t *relative;
    ULONG used = 0, i;
    DWORD got, r;
    HANDLE h;
    long n;
    wchar_t *end = NULL;
    BOOL found = FALSE;

    if (_wcsnicmp(physical, prefix, prefix_len) != 0)
        return fail(rep, STAGE_GUARD_REAL, ERROR_ACCESS_DENIED, L"not a physical drive path");
    n = wcstol(physical + prefix_len, &end, 10);
    if (end == physical + prefix_len || *end != L'\0' || n < 0 || n >= MAX_DRIVES)
        return fail(rep, STAGE_GUARD_REAL, ERROR_ACCESS_DENIED, L"bad drive number");

    if (!(skip & GUARD_SKIP_REAL) && g_real_drive[n])
        return fail(rep, STAGE_GUARD_REAL, ERROR_ACCESS_DENIED, L"drive existed at helper start");

    h = CreateFileW(physical, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return fail(rep, STAGE_GUARD_NUMBER, GetLastError(), L"cannot open drive to check it");

    if (!(skip & GUARD_SKIP_NUMBER)) {
        if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &number, sizeof(number), &got, NULL) ||
            number.DeviceNumber != (DWORD)n) {
            CloseHandle(h);
            return fail(rep, STAGE_GUARD_NUMBER, ERROR_ACCESS_DENIED, L"device number mismatch");
        }
    }

    if (!(skip & GUARD_SKIP_BUS)) {
        ZeroMemory(&query, sizeof(query));
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        ZeroMemory(desc_buf, sizeof(desc_buf));
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                             desc_buf, sizeof(desc_buf), &got, NULL) ||
            desc->BusType != BusTypeFileBackedVirtual) {
            CloseHandle(h);
            return fail(rep, STAGE_GUARD_BUS, ERROR_ACCESS_DENIED, L"not a file-backed virtual disk");
        }
    }

    if (!(skip & GUARD_SKIP_LENGTH)) {
        if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length), &got, NULL) ||
            (ULONGLONG)length.Length.QuadPart != HELPER_DISK_SIZE) {
            CloseHandle(h);
            return fail(rep, STAGE_GUARD_LENGTH, ERROR_ACCESS_DENIED, L"not the size of a test disk");
        }
    }

    /* The disk's own backing file has to be the one this request attached. */
    relative = wcschr(disk, L'\\');
    ZeroMemory(dep_buf, sizeof(dep_buf));
    dep->Version = STORAGE_DEPENDENCY_INFO_VERSION_2;
    r = GetStorageDependencyInformation(h,
        GET_STORAGE_DEPENDENCY_FLAG_DISK_HANDLE | GET_STORAGE_DEPENDENCY_FLAG_HOST_VOLUMES,
        sizeof(dep_buf), dep, &used);
    CloseHandle(h);
    if (r != ERROR_SUCCESS)
        return fail(rep, STAGE_GUARD_BACKING, r, L"cannot read the drive's backing file");
    for (i = 0; relative && i < dep->NumberEntries; i++) {
        const wchar_t *dependent = dep->Version2Entries[i].DependentVolumeRelativePath;
        if (dependent && _wcsicmp(dependent, relative) == 0)
            found = TRUE;
    }
    if (!found)
        return fail(rep, STAGE_GUARD_BACKING, ERROR_ACCESS_DENIED, L"backing file is not this disk");

    return ERROR_SUCCESS;
}

static DWORD attach_disk(const wchar_t *disk, BOOL read_only, HANDLE *out, HelperReply *rep)
{
    VIRTUAL_STORAGE_TYPE st;
    OPEN_VIRTUAL_DISK_PARAMETERS open_params;
    ATTACH_VIRTUAL_DISK_PARAMETERS attach_params;
    GET_VIRTUAL_DISK_INFO info;
    ULONG info_size = sizeof(info);
    HANDLE vhd = INVALID_HANDLE_VALUE;
    ATTACH_VIRTUAL_DISK_FLAG flags = ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
    DWORD r;

    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    st.VendorId = VENDOR_MS;
    ZeroMemory(&open_params, sizeof(open_params));
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    open_params.Version2.ReadOnly = read_only;
    r = OpenVirtualDisk(&st, disk, VIRTUAL_DISK_ACCESS_NONE, OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params, &vhd);
    if (r != ERROR_SUCCESS)
        return fail(rep, STAGE_OPEN, r, L"cannot open virtual disk");

    ZeroMemory(&info, sizeof(info));
    info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
    r = GetVirtualDiskInformation(vhd, &info_size, &info, NULL);
    if (r != ERROR_SUCCESS || info.Size.VirtualSize != HELPER_DISK_SIZE) {
        CloseHandle(vhd);
        return fail(rep, STAGE_SIZE, r ? r : ERROR_ACCESS_DENIED, L"not a test-sized disk");
    }

    if (read_only) flags |= ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY;
    ZeroMemory(&attach_params, sizeof(attach_params));
    attach_params.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    r = AttachVirtualDisk(vhd, NULL, flags, 0, &attach_params, NULL);
    if (r != ERROR_SUCCESS) {
        CloseHandle(vhd);
        return fail(rep, STAGE_ATTACH, r, L"cannot attach virtual disk");
    }
    *out = vhd;
    return ERROR_SUCCESS;
}

static void detach_disk(HANDLE vhd)
{
    int tries;
    for (tries = 0; tries < 40; tries++) {
        if (DetachVirtualDisk(vhd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0) == ERROR_SUCCESS) break;
        Sleep(50);
    }
    CloseHandle(vhd);
}

static DWORD physical_path(HANDLE vhd, wchar_t *out, ULONG out_chars, HelperReply *rep)
{
    DWORD r = ERROR_SUCCESS;
    int tries;
    for (tries = 0; tries < 40; tries++) {
        ULONG size = out_chars * sizeof(wchar_t);
        r = GetVirtualDiskPhysicalPath(vhd, &size, out);
        if (r == ERROR_SUCCESS) return r;
        Sleep(50);
    }
    return fail(rep, STAGE_PHYSICAL, r, L"cannot find the attached drive");
}

static BOOL record_ok(ULONGLONG offset)
{
    return offset % HELPER_SECTOR == 0 && offset + HELPER_SECTOR <= HELPER_DISK_SIZE;
}

static DWORD do_records(const HelperRequest *req, HelperReply *rep, const wchar_t *physical, BOOL write)
{
    /* Unbuffered I/O: page-aligned buffer, whole sectors. */
    BYTE *sector = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE h;
    DWORD i, done, r = ERROR_SUCCESS;

    if (!sector) return fail(rep, STAGE_IO, ERROR_OUTOFMEMORY, L"no memory");
    h = CreateFileW(physical, write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        r = fail(rep, STAGE_IO, GetLastError(), L"cannot open drive");
        VirtualFree(sector, 0, MEM_RELEASE);
        return r;
    }
    for (i = 0; i < req->count && r == ERROR_SUCCESS; i++) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)req->offset[i];
        ZeroMemory(sector, HELPER_SECTOR);
        if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) {
            r = fail(rep, STAGE_IO, GetLastError(), L"seek failed");
        } else if (write) {
            CopyMemory(sector, req->data[i], HELPER_RECORD_SIZE);
            if (!WriteFile(h, sector, HELPER_SECTOR, &done, NULL) || done != HELPER_SECTOR)
                r = fail(rep, STAGE_IO, GetLastError(), L"write failed");
        } else {
            if (!ReadFile(h, sector, HELPER_SECTOR, &done, NULL) || done != HELPER_SECTOR)
                r = fail(rep, STAGE_IO, GetLastError(), L"read failed");
            else
                CopyMemory(rep->data[i], sector, HELPER_RECORD_SIZE);
        }
    }
    if (r == ERROR_SUCCESS && write && !FlushFileBuffers(h))
        r = fail(rep, STAGE_IO, GetLastError(), L"flush failed");
    CloseHandle(h);
    VirtualFree(sector, 0, MEM_RELEASE);
    return r;
}

static void handle_request(const HelperRequest *req, HelperReply *rep)
{
    wchar_t disk[HELPER_MAX_PATH];
    wchar_t physical[MAX_PATH];
    HANDLE vhd = INVALID_HANDLE_VALUE;
    DWORD i;
    BOOL read_only = (req->flags & HELPER_FLAG_READ_ONLY) != 0;

    switch (req->op) {
    case OP_PING:
        rep->version = HELPER_VERSION;
        wcscpy_s(rep->text, HELPER_MAX_PATH, g_root);
        return;

    case OP_RELEASE:
        if (g_held != INVALID_HANDLE_VALUE) {
            detach_disk(g_held);
            g_held = INVALID_HANDLE_VALUE;
        }
        return;

    case OP_QUIT:
        return;

    case OP_GUARD_PROBE:
        /* A real drive is opened read-only here, and nothing on this path can write. */
        if (req->probe_drive >= MAX_DRIVES) {
            fail(rep, STAGE_REQUEST, ERROR_INVALID_PARAMETER, L"bad drive number");
            return;
        }
        if (resolve_disk(req->path, disk, HELPER_MAX_PATH, rep) != ERROR_SUCCESS) return;
        swprintf_s(physical, MAX_PATH, L"\\\\.\\PhysicalDrive%lu", req->probe_drive);
        guard_check(physical, disk, req->probe_skip, rep);
        return;

    case OP_WRITE:
    case OP_READ:
        if (req->count == 0 || req->count > HELPER_MAX_RECORDS) {
            fail(rep, STAGE_REQUEST, ERROR_INVALID_PARAMETER, L"bad record count");
            return;
        }
        for (i = 0; i < req->count; i++) {
            if (!record_ok(req->offset[i])) {
                fail(rep, STAGE_REQUEST, ERROR_INVALID_PARAMETER, L"bad record offset");
                return;
            }
        }
        if (resolve_disk(req->path, disk, HELPER_MAX_PATH, rep) != ERROR_SUCCESS) return;
        if (attach_disk(disk, req->op == OP_READ, &vhd, rep) != ERROR_SUCCESS) return;
        if (physical_path(vhd, physical, MAX_PATH, rep) == ERROR_SUCCESS &&
            guard_check(physical, disk, 0, rep) == ERROR_SUCCESS)
            do_records(req, rep, physical, req->op == OP_WRITE);
        detach_disk(vhd);
        return;

    case OP_TRY_ATTACH:
        if (resolve_disk(req->path, disk, HELPER_MAX_PATH, rep) != ERROR_SUCCESS) return;
        if (attach_disk(disk, read_only, &vhd, rep) != ERROR_SUCCESS) return;
        detach_disk(vhd);
        return;

    case OP_HOLD:
        if (g_held != INVALID_HANDLE_VALUE) {
            fail(rep, STAGE_REQUEST, ERROR_BUSY, L"already holding a disk");
            return;
        }
        if (resolve_disk(req->path, disk, HELPER_MAX_PATH, rep) != ERROR_SUCCESS) return;
        if (attach_disk(disk, read_only, &vhd, rep) != ERROR_SUCCESS) return;
        g_held = vhd;
        return;

    default:
        fail(rep, STAGE_REQUEST, ERROR_INVALID_FUNCTION, L"unknown operation");
    }
}

static DWORD WINAPI idle_watch(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(60 * 1000);
        if (GetTickCount64() - g_last_request > HELPER_IDLE_MS) {
            wprintf(L"Idle for too long, exiting.\n");
            ExitProcess(0);
        }
    }
}

static BOOL is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION elevation;
    DWORD got = 0;
    BOOL result = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &got))
            result = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return result;
}

/* This user only, and a Medium label: a High process's objects otherwise refuse the
   unelevated tests this exists to serve. */
static BOOL make_pipe_security(SECURITY_ATTRIBUTES *sa)
{
    HANDLE token;
    BYTE user_buf[256];
    TOKEN_USER *user = (TOKEN_USER *)user_buf;
    DWORD got = 0;
    wchar_t *sid = NULL;
    wchar_t sddl[256];
    PSECURITY_DESCRIPTOR sd = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    if (!GetTokenInformation(token, TokenUser, user_buf, sizeof(user_buf), &got)) {
        CloseHandle(token);
        return FALSE;
    }
    CloseHandle(token);
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) return FALSE;
    swprintf_s(sddl, 256, L"D:P(A;;GA;;;%s)S:(ML;;NW;;;ME)", sid);
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL))
        return FALSE;
    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = sd;
    sa->bInheritHandle = FALSE;
    return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE pipe;
    wchar_t log_path[MAX_PATH];
    wchar_t name[64];
    int i;
    int exit_code = 1;

    /* It runs elevated from a folder the user can write to: no DLLs from here. */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetConsoleTitleW(L"AppSandbox snapshot tests: disk helper");

    if (argc != 2) {
        wprintf(L"usage: asb-disk-helper <test folder>\n");
        goto wait;
    }
    if (!is_elevated()) {
        wprintf(L"This helper has to run elevated: use start-helper.cmd.\n");
        goto wait;
    }
    if (!final_path(argv[1], FILE_FLAG_BACKUP_SEMANTICS, g_root, MAX_PATH) || wcslen(g_root) <= 3) {
        wprintf(L"The test folder does not exist, or is a drive root: %s\n", argv[1]);
        goto wait;
    }

    for (i = 0; i < MAX_DRIVES; i++) {
        HANDLE h;
        swprintf_s(name, 64, L"\\\\.\\PhysicalDrive%d", i);
        h = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            g_real_drive[i] = TRUE;
            CloseHandle(h);
        }
    }

    if (!make_pipe_security(&sa)) {
        wprintf(L"Cannot build the pipe's security (error %lu).\n", GetLastError());
        goto wait;
    }
    pipe = CreateNamedPipeW(HELPER_PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, sizeof(HelperReply), sizeof(HelperRequest), 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
        wprintf(L"Cannot create the pipe (error %lu). Is another helper running?\n", GetLastError());
        goto wait;
    }

    swprintf_s(log_path, MAX_PATH, L"%s\\helper.log", g_root);
    _wfopen_s(&g_log, log_path, L"a, ccs=UTF-8");

    g_last_request = GetTickCount64();
    CreateThread(NULL, 0, idle_watch, NULL, 0, NULL);

    wprintf(L"Disk helper running. Only disks inside this folder can be used:\n  %s\n", g_root);
    wprintf(L"Close this window to stop it. It also stops by itself after 12 idle hours.\n");

    for (;;) {
        static HelperRequest req;
        static HelperReply rep;
        DWORD got = 0, sent = 0;
        ULONGLONG started;
        BOOL quit = FALSE;

        if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
            continue;
        ZeroMemory(&rep, sizeof(rep));
        if (ReadFile(pipe, &req, sizeof(req), &got, NULL) && got == sizeof(req)) {
            req.path[HELPER_MAX_PATH - 1] = L'\0';
            g_last_request = started = GetTickCount64();
            handle_request(&req, &rep);
            quit = req.op == OP_QUIT;
            if (g_log) {
                fwprintf(g_log, L"op=%lu stage=%lu error=%lu ms=%llu %s\n", req.op, rep.stage, rep.error,
                         GetTickCount64() - started, req.path);
                fflush(g_log);
            }
            WriteFile(pipe, &rep, sizeof(rep), &sent, NULL);
            FlushFileBuffers(pipe);
        }
        DisconnectNamedPipe(pipe);
        if (quit) {
            exit_code = 0;
            break;
        }
    }
    if (g_held != INVALID_HANDLE_VALUE) detach_disk(g_held);
    return exit_code;

wait:
    wprintf(L"Press Enter to close.\n");
    getwchar();
    return exit_code;
}
