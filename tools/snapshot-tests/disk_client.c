#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>
#include <stdio.h>
#include <stdlib.h>
#include "disk_client.h"

#pragma comment(lib, "virtdisk.lib")

static const GUID VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

DWORD disk_call(const HelperRequest *req, HelperReply *rep)
{
    DWORD got = 0;
    if (!CallNamedPipeW(HELPER_PIPE_NAME, (void *)req, sizeof(*req), rep, sizeof(*rep), &got, 120000))
        return GetLastError();
    return got == sizeof(*rep) ? ERROR_SUCCESS : ERROR_BAD_LENGTH;
}

static DWORD ask(const HelperRequest *req, HelperReply *rep)
{
    DWORD r = disk_call(req, rep);
    if (r != ERROR_SUCCESS) {
        /* Not a test result: without the helper this run proves nothing. */
        fwprintf(stderr, L"\nFATAL: lost the disk helper (error %lu). Run aborted, not a test failure.\n", r);
        fflush(stderr);
        ExitProcess(3);
    }
    return rep->error;
}

static HelperRequest *new_request(DWORD op, const wchar_t *vhdx)
{
    static HelperRequest req;
    ZeroMemory(&req, sizeof(req));
    req.op = op;
    if (vhdx) wcscpy_s(req.path, HELPER_MAX_PATH, vhdx);
    return &req;
}

DWORD disk_write(const wchar_t *vhdx, ULONGLONG offset, const char *text, HelperReply *rep)
{
    HelperRequest *req = new_request(OP_WRITE, vhdx);
    req->count = 1;
    req->offset[0] = offset;
    strncpy_s((char *)req->data[0], HELPER_RECORD_SIZE, text, _TRUNCATE);
    return ask(req, rep);
}

DWORD disk_read_many(const wchar_t *vhdx, const ULONGLONG *offset, DWORD count, HelperReply *rep)
{
    HelperRequest *req = new_request(OP_READ, vhdx);
    DWORD i;
    req->count = count;
    for (i = 0; i < count; i++) req->offset[i] = offset[i];
    return ask(req, rep);
}

DWORD disk_read(const wchar_t *vhdx, ULONGLONG offset, char *text, size_t text_len, HelperReply *rep)
{
    DWORD r = disk_read_many(vhdx, &offset, 1, rep);
    text[0] = '\0';
    if (r == ERROR_SUCCESS)
        strncpy_s(text, text_len, (const char *)rep->data[0], _TRUNCATE);
    return r;
}

DWORD disk_try_attach(const wchar_t *vhdx, BOOL read_only, HelperReply *rep)
{
    HelperRequest *req = new_request(OP_TRY_ATTACH, vhdx);
    req->flags = read_only ? HELPER_FLAG_READ_ONLY : 0;
    return ask(req, rep);
}

DWORD disk_hold(const wchar_t *vhdx, BOOL read_only, HelperReply *rep)
{
    HelperRequest *req = new_request(OP_HOLD, vhdx);
    req->flags = read_only ? HELPER_FLAG_READ_ONLY : 0;
    return ask(req, rep);
}

DWORD disk_release(HelperReply *rep)
{
    return ask(new_request(OP_RELEASE, NULL), rep);
}

DWORD disk_guard_probe(const wchar_t *vhdx, DWORD drive, DWORD skip, HelperReply *rep)
{
    HelperRequest *req = new_request(OP_GUARD_PROBE, vhdx);
    req->probe_drive = drive;
    req->probe_skip = skip;
    return ask(req, rep);
}

/* ---- Self-test: the helper's fences, each seen refusing ---- */

static int g_failed;

static void expect(BOOL ok, const wchar_t *what, const HelperReply *rep)
{
    wprintf(L"  %s  %s", ok ? L"ok  " : L"FAIL", what);
    if (rep) wprintf(L"  [stage %lu error %lu]", rep->stage, rep->error);
    wprintf(L"\n");
    if (!ok) g_failed++;
}

static BOOL make_disk(const wchar_t *path, const wchar_t *parent, ULONGLONG size)
{
    VIRTUAL_STORAGE_TYPE st;
    CREATE_VIRTUAL_DISK_PARAMETERS p;
    HANDLE h = INVALID_HANDLE_VALUE;
    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    st.VendorId = VENDOR_MS;
    ZeroMemory(&p, sizeof(p));
    p.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    p.Version2.MaximumSize = size;
    p.Version2.ParentPath = parent;
    p.Version2.SectorSizeInBytes = parent ? 0 : 512;
    p.Version2.PhysicalSectorSizeInBytes = parent ? 0 : 4096;
    if (CreateVirtualDisk(&st, path, VIRTUAL_DISK_ACCESS_NONE, NULL,
                          CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &p, NULL, &h) != ERROR_SUCCESS)
        return FALSE;
    CloseHandle(h);
    return TRUE;
}

int disk_helper_selftest(const wchar_t *folder)
{
    static const ULONGLONG OFFSET = 1024 * 1024;
    HelperRequest *req;
    HelperReply rep;
    wchar_t dir[MAX_PATH], base[MAX_PATH], child[MAX_PATH], big[MAX_PATH];
    wchar_t outside_dir[MAX_PATH], outside[MAX_PATH], junction[MAX_PATH], via[MAX_PATH], wrong[MAX_PATH];
    wchar_t cmd[MAX_PATH * 3];
    char text[HELPER_RECORD_SIZE];
    DWORD drive;
    DWORD r;
    int real = 0;

    g_failed = 0;
    ZeroMemory(&rep, sizeof(rep));

    req = new_request(OP_PING, NULL);
    r = disk_call(req, &rep);
    if (r != ERROR_SUCCESS) {
        wprintf(L"The disk helper is not running (error %lu). Start it with start-helper.cmd.\n", r);
        return -1;
    }
    wprintf(L"helper version %lu, test folder %s\n", rep.version, rep.text);
    expect(rep.version == HELPER_VERSION, L"helper is the expected version", NULL);
    if (_wcsnicmp(folder, rep.text, wcslen(rep.text)) != 0) {
        wprintf(L"The running helper only serves the folder above, not %s.\n"
                L"Close its window and try again.\n", folder);
        return -1;
    }

    swprintf_s(dir, MAX_PATH, L"%s\\selftest-%lu", folder, GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    swprintf_s(base, MAX_PATH, L"%s\\base.vhdx", dir);
    swprintf_s(child, MAX_PATH, L"%s\\child.vhdx", dir);
    expect(make_disk(base, NULL, HELPER_DISK_SIZE), L"create a test disk, unelevated", NULL);
    expect(make_disk(child, base, 0), L"create a differencing child, unelevated", NULL);

    wprintf(L"Reads and writes go through the layers:\n");
    expect(disk_write(child, OFFSET, "selftest marker", &rep) == 0, L"write through the child", &rep);
    disk_read(child, OFFSET, text, sizeof(text), &rep);
    expect(strcmp(text, "selftest marker") == 0, L"the child reads the marker back", &rep);
    disk_read(base, OFFSET, text, sizeof(text), &rep);
    expect(text[0] == '\0', L"the parent does not have it", &rep);

    wprintf(L"Refused outright:\n");
    swprintf_s(big, MAX_PATH, L"%s\\wrong-size.vhdx", dir);
    expect(make_disk(big, NULL, HELPER_DISK_SIZE * 2), L"create a disk of the wrong size", NULL);
    r = disk_write(big, OFFSET, "x", &rep);
    expect(r != 0 && rep.stage == STAGE_SIZE, L"wrong-size disk", &rep);

    GetTempPathW(MAX_PATH, outside_dir);
    wcscat_s(outside_dir, MAX_PATH, L"asb-selftest-outside");
    CreateDirectoryW(outside_dir, NULL);
    swprintf_s(outside, MAX_PATH, L"%s\\outside.vhdx", outside_dir);
    make_disk(outside, NULL, HELPER_DISK_SIZE);
    r = disk_write(outside, OFFSET, "x", &rep);
    expect(r != 0 && rep.stage == STAGE_PATH, L"disk outside the test folder", &rep);

    swprintf_s(junction, MAX_PATH, L"%s\\junction", dir);
    swprintf_s(cmd, MAX_PATH * 3, L"cmd /c mklink /J \"%s\" \"%s\" >nul", junction, outside_dir);
    _wsystem(cmd);
    swprintf_s(via, MAX_PATH, L"%s\\outside.vhdx", junction);
    expect(GetFileAttributesW(via) != INVALID_FILE_ATTRIBUTES, L"junction out of the folder exists", NULL);
    r = disk_write(via, OFFSET, "x", &rep);
    expect(r != 0 && rep.stage == STAGE_PATH, L"disk reached through that junction", &rep);

    swprintf_s(wrong, MAX_PATH, L"%s\\not-a-disk.txt", dir);
    CloseHandle(CreateFileW(wrong, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    r = disk_write(wrong, OFFSET, "x", &rep);
    expect(r != 0 && rep.stage == STAGE_PATH, L"file that is not a .vhdx", &rep);

    r = disk_write(child, OFFSET + 1, "x", &rep);
    expect(r != 0 && rep.stage == STAGE_REQUEST, L"unaligned offset", &rep);

    wprintf(L"The raw-disk guard refuses every real drive, one check at a time. Nothing here can write.\n");
    for (drive = 0; drive < 64; drive++) {
        wchar_t name[64], what[128];
        HANDLE h;
        swprintf_s(name, 64, L"\\\\.\\PhysicalDrive%lu", drive);
        h = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        real++;

        r = disk_guard_probe(child, drive, 0, &rep);
        swprintf_s(what, 128, L"drive %lu: all checks on", drive);
        expect(r != 0 && rep.stage == STAGE_GUARD_REAL, what, &rep);

        r = disk_guard_probe(child, drive, GUARD_SKIP_REAL, &rep);
        swprintf_s(what, 128, L"drive %lu: without the known-drives check", drive);
        expect(r != 0 && rep.stage > STAGE_GUARD_REAL, what, &rep);

        r = disk_guard_probe(child, drive, GUARD_SKIP_REAL | GUARD_SKIP_NUMBER | GUARD_SKIP_BUS, &rep);
        swprintf_s(what, 128, L"drive %lu: without known-drives, number and bus checks", drive);
        expect(r != 0 && rep.stage >= STAGE_GUARD_LENGTH, what, &rep);

        r = disk_guard_probe(child, drive, GUARD_SKIP_REAL | GUARD_SKIP_NUMBER | GUARD_SKIP_BUS | GUARD_SKIP_LENGTH, &rep);
        swprintf_s(what, 128, L"drive %lu: only the backing-file check left", drive);
        expect(r != 0 && rep.stage == STAGE_GUARD_BACKING, what, &rep);
    }
    expect(real > 0, L"found at least one real drive to try", NULL);

    /* The junction first: removing it must not follow it. */
    RemoveDirectoryW(junction);
    DeleteFileW(wrong);
    DeleteFileW(big);
    DeleteFileW(child);
    DeleteFileW(base);
    RemoveDirectoryW(dir);
    DeleteFileW(outside);
    RemoveDirectoryW(outside_dir);

    wprintf(L"selftest: %d failed\n", g_failed);
    return g_failed;
}
