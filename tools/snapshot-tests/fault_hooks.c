#include <windows.h>
#include <virtdisk.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <io.h>
#include <wchar.h>
#include "disk_util.h"
#include "fault_control.h"

#pragma comment(lib, "virtdisk.lib")

static const GUID VHDX_VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

/* The product's own function, cut from its disk_util.c at build time: that file also
   needs the UI and the provisioning code. */
#include "vhdx_create_differencing.inc"

static BOOL g_armed;
static int g_count;
static int g_site;
static FaultAction g_action;
static BOOL g_failing_from_now_on;
static BOOL g_kill_when_done;
static BOOL g_stream_failed;
static FILE *g_trace;
static wchar_t g_fence[MAX_PATH];
static const char *g_last_op = "none";

void fault_set_fence(const wchar_t *folder)
{
    wcscpy_s(g_fence, MAX_PATH, folder);
}

void fault_arm(int site, FaultAction action, const wchar_t *trace_path)
{
    g_armed = TRUE;
    g_count = 0;
    g_site = site;
    g_action = action;
    g_failing_from_now_on = FALSE;
    g_kill_when_done = FALSE;
    g_stream_failed = FALSE;
    if (trace_path) _wfopen_s(&g_trace, trace_path, L"w");
}

void fault_disarm(void)
{
    g_armed = FALSE;
    if (g_trace) {
        fclose(g_trace);
        g_trace = NULL;
    }
}

/* TerminateProcess, not ExitProcess: DLL detach would flush the CRT's stream buffers,
   which a real kill does not, and would make a crash mid-save look better than it is. */
static void die(void)
{
    TerminateProcess(GetCurrentProcess(), EXIT_KILLED);
}

static void check_fence(const wchar_t *path)
{
    size_t n = wcslen(g_fence);
    if (n == 0 || _wcsnicmp(path, g_fence, n) != 0 || path[n] != L'\\') {
        fwprintf(stderr, L"FATAL: file operation outside the test folder: %s\n", path);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), EXIT_FENCE);
    }
}

const char *fault_last_op(void)
{
    return g_last_op;
}

BOOL fault_point(const char *name)
{
    g_last_op = name;
    if (!g_armed) return FALSE;
    g_count++;
    if (g_trace) {
        fprintf(g_trace, "%d %s\n", g_count, name);
        fflush(g_trace);
    }
    if (g_failing_from_now_on) return TRUE;
    if (g_count != g_site) return FALSE;
    switch (g_action) {
    case FAULT_FAIL:
        return TRUE;
    case FAULT_FAIL_ALL:
        g_failing_from_now_on = TRUE;
        return TRUE;
    case FAULT_KILL_BEFORE:
        die();
        return FALSE;
    case FAULT_KILL_AFTER:
        g_kill_when_done = TRUE;
        return FALSE;
    default:
        return FALSE;
    }
}

void fault_point_done(void)
{
    if (g_kill_when_done) die();
}

BOOL hook_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    BOOL ok;
    check_fence(from);
    check_fence(to);
    if (fault_point("MoveFileExW")) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    ok = MoveFileExW(from, to, flags);
    fault_point_done();
    return ok;
}

BOOL hook_DeleteFileW(LPCWSTR path)
{
    BOOL ok;
    check_fence(path);
    if (fault_point("DeleteFileW")) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    ok = DeleteFileW(path);
    fault_point_done();
    return ok;
}

UINT hook_GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR out)
{
    UINT r;
    check_fence(dir);
    if (fault_point("GetTempFileNameW")) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    r = GetTempFileNameW(dir, prefix, unique, out);
    fault_point_done();
    return r;
}

HANDLE hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                        DWORD disposition, DWORD flags, HANDLE templ)
{
    HANDLE h;
    check_fence(path);
    if (fault_point("CreateFileW")) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    h = CreateFileW(path, access, share, sa, disposition, flags, templ);
    fault_point_done();
    return h;
}

BOOL hook_FlushFileBuffers(HANDLE h)
{
    BOOL ok;
    if (fault_point("FlushFileBuffers")) {
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    ok = FlushFileBuffers(h);
    fault_point_done();
    return ok;
}

errno_t hook_wfopen_s(FILE **f, const wchar_t *path, const wchar_t *mode)
{
    errno_t e;
    if (wcschr(mode, L'w')) check_fence(path);
    if (fault_point("_wfopen_s")) {
        *f = NULL;
        return EACCES;
    }
    e = _wfopen_s(f, path, mode);
    fault_point_done();
    return e;
}

/* A failed write loses that text and, as on a full disk, everything after it. */
int hook_fwprintf(FILE *f, const wchar_t *format, ...)
{
    va_list args;
    int n;
    if (g_stream_failed) return -1;
    if (fault_point("fwprintf")) {
        g_stream_failed = TRUE;
        return -1;
    }
    va_start(args, format);
    n = vfwprintf(f, format, args);
    va_end(args);
    fault_point_done();
    return n;
}

int hook_ferror(FILE *f)
{
    return g_stream_failed || ferror(f);
}

int hook_fflush(FILE *f)
{
    int r;
    if (fault_point("fflush")) return EOF;
    r = fflush(f);
    fault_point_done();
    return r;
}

int hook_commit(int fd)
{
    int r;
    if (fault_point("_commit")) return -1;
    r = _commit(fd);
    fault_point_done();
    return r;
}

/* A failed close still closes: the file must not stay open behind the test's back. */
int hook_fclose(FILE *f)
{
    int r;
    BOOL fail = fault_point("fclose");
    r = fclose(f);
    fault_point_done();
    return fail ? EOF : r;
}

HRESULT vhdx_create_differencing_for_tests(const wchar_t *child_path, const wchar_t *parent_path)
{
    return vhdx_create_differencing(child_path, parent_path);
}

HRESULT hook_vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path)
{
    HRESULT hr;
    check_fence(child_path);
    if (fault_point("vhdx_create_differencing"))
        return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    hr = vhdx_create_differencing(child_path, parent_path);
    fault_point_done();
    return hr;
}
