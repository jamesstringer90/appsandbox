/* Failure-recovery tests for snapshot.c, on tiny real VHDX chains and with no VM.

   For each scenario, a first run records every file operation the code under test
   performs. Each operation is then made to fail, to fail along with everything after
   it, and to be preceded and followed by a kill. A fresh process then looks at what was
   left, as a restart would. The same source builds against any version of snapshot.c,
   so nothing here depends on knowing what a given version does.

   See README.md. */
#include <windows.h>
#include <virtdisk.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <io.h>
#include "snapshot.h"
#include "disk_client.h"
#include "fault_control.h"

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

/* A version without snapshot_leave_disks has no step before the delete: the linker then
   takes no_leave_step instead, so that one source builds against every version. */
HRESULT snapshot_leave_disks(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx);
HRESULT no_leave_step(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx)
{
    (void)tree; (void)instance; (void)index; (void)branch_idx;
    return S_FALSE;
}
#pragma comment(linker, "/alternatename:snapshot_leave_disks=no_leave_step")

static const GUID VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

#define EXIT_FAILED_CLEAN 20 /* the operation reported failure, memory unchanged */
#define EXIT_FAILED_DIRTY 21 /* the operation reported failure, memory changed */
#define EXIT_FAILED_VM_UNSAVED 22 /* the operation reported failure, memory unchanged, vms.cfg not saved */
#define EXIT_DONE_VM_UNSAVED 30 /* it reported success, but saving vms.cfg then failed */

#define MARKER_STEP (1024ULL * 1024)
#define MAX_MARKERS 24
#define MAX_FILES   64
#define MAX_NAMED   64
#define MAX_SITES   256

enum { OP_DO_TAKE, OP_DO_NEW_BRANCH, OP_DO_START, OP_DO_DELETE_BRANCH, OP_DO_DELETE };

enum {
    CHECK_CONTENT,
    CHECK_NO_LOSS,
    CHECK_CHAIN,
    CHECK_METADATA,
    CHECK_IMMUTABLE,
    CHECK_NO_RENAME,
    CHECK_HONEST,
    CHECK_USABLE,
    CHECK_COUNT
};

static const wchar_t *CHECK_NAME[CHECK_COUNT] = {
    L"content", L"no-loss", L"chain", L"metadata", L"immutable", L"no-rename", L"honest", L"usable"
};

static const wchar_t *CHECK_MEANING[CHECK_COUNT] = {
    L"the VM's disk, after a restart, shows exactly what it should",
    L"everything written is still reachable from a disk the metadata names",
    L"parent links match the metadata, no working branch is anyone's parent, every disk attaches",
    L"every path named exists, the VM's disk is known, no snapshot was forgotten",
    L"frozen disks do not change, and the VM never runs on one",
    L"no existing disk file disappeared from under its name",
    L"success means it happened; failure means nothing changed, on disk or in memory",
    L"after the event, the VM can write, and the next snapshot works"
};

typedef struct {
    wchar_t dir[MAX_PATH];
    wchar_t snap_dir[MAX_PATH];
    wchar_t base[MAX_PATH];
    wchar_t vm_file[MAX_PATH];
    int     markers;
} World;

typedef struct {
    const wchar_t *name;
    void (*setup)(World *w);
    int op;
    int arg;
    int arg2;
} Scenario;

typedef struct {
    BOOL    failed[CHECK_COUNT];
    wchar_t why[CHECK_COUNT][260];
} Result;

typedef struct {
    wchar_t path[MAX_PATH];
    BOOL    is_branch;
    int     snap;   /* index of the snapshot, or -2 for the base */
} Named;

typedef struct {
    wchar_t  active[MAX_PATH];
    unsigned visible;            /* markers the VM's disk showed */
    unsigned visible_target;     /* markers a new branch of the op's target would show */
    int      nfiles;
    wchar_t  files[MAX_FILES][MAX_PATH];
    BYTE     hash[MAX_FILES][32];
    int      nsnaps;
    wchar_t  snap_names[MAX_SNAPSHOTS][128];
    int      nnamed;
    Named    named[MAX_NAMED];
    BOOL     had_base;
    /* What a delete operation is meant to remove */
    int      ndoomed;
    wchar_t  doomed[MAX_BRANCHES + 1][MAX_PATH];
    wchar_t  doomed_snapshot[128];
    unsigned must_survive;       /* markers that have to stay reachable */
} Pre;

static wchar_t g_run_dir[MAX_PATH];
static wchar_t g_exe[MAX_PATH];
static FILE *g_tsv;
static FILE *g_summary;
static int g_cases;
static int g_planned;
static BOOL g_keep;
static int g_check_failed[CHECK_COUNT];
static int g_cases_failed;
static const wchar_t *g_filter;
static int g_shell_pauses;
static ULONGLONG g_shell_paused_ms;

/* ---- Small helpers ---- */

static void fatal(const wchar_t *format, ...)
{
    va_list args;
    fwprintf(stderr, L"\nFATAL (run aborted, not a test result): ");
    va_start(args, format);
    vfwprintf(stderr, format, args);
    va_end(args);
    fwprintf(stderr, L"\n");
    fflush(stderr);
    ExitProcess(4);
}

static void say(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    vwprintf(format, args);
    va_end(args);
    fflush(stdout);
    if (g_summary) {
        va_start(args, format);
        vfwprintf(g_summary, format, args);
        va_end(args);
        fflush(g_summary);
    }
}

static BOOL exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static const wchar_t *leaf(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    if (!path[0]) return L"(an empty path)";
    return slash ? slash + 1 : path;
}

static BOOL same_path(const wchar_t *a, const wchar_t *b)
{
    if (wcsncmp(a, L"\\\\?\\", 4) == 0) a += 4;
    if (wcsncmp(b, L"\\\\?\\", 4) == 0) b += 4;
    return _wcsicmp(a, b) == 0;
}

static void note(Result *r, int check, const wchar_t *format, ...)
{
    va_list args;
    if (r->failed[check]) return;
    r->failed[check] = TRUE;
    va_start(args, format);
    vswprintf_s(r->why[check], 260, format, args);
    va_end(args);
}

static void mask_text(unsigned mask, wchar_t *out, size_t out_len)
{
    int i;
    out[0] = L'\0';
    for (i = 0; i < MAX_MARKERS; i++) {
        if (mask & (1u << i)) {
            wchar_t one[8];
            swprintf_s(one, 8, L"M%d ", i);
            wcscat_s(out, out_len, one);
        }
    }
    if (out[0] == L'\0') wcscpy_s(out, out_len, L"nothing");
}

static BOOL hash_file(const wchar_t *path, BYTE out[32])
{
    static BCRYPT_ALG_HANDLE alg;
    HANDLE h = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    BYTE *data;
    DWORD got;
    int tries;
    BOOL ok;

    if (!alg && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        fatal(L"no SHA-256 provider");
    for (tries = 0; tries < 40; tries++) {
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
        Sleep(50);
    }
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(h);
        return FALSE;
    }
    data = (BYTE *)malloc((size_t)size.QuadPart + 1);
    if (!data) fatal(L"out of memory");
    ok = ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart;
    CloseHandle(h);
    if (ok) ok = BCryptHash(alg, NULL, 0, data, got, out, 32) == 0;
    free(data);
    return ok;
}

/* Base disks are made the way vhdx_create makes them, at the size the helper insists on. */
static void make_base(const wchar_t *path)
{
    VIRTUAL_STORAGE_TYPE st;
    CREATE_VIRTUAL_DISK_PARAMETERS p;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD r;

    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    st.VendorId = VENDOR_MS;
    ZeroMemory(&p, sizeof(p));
    p.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    p.Version2.MaximumSize = HELPER_DISK_SIZE;
    p.Version2.SectorSizeInBytes = 512;
    p.Version2.PhysicalSectorSizeInBytes = 4096;
    r = CreateVirtualDisk(&st, path, VIRTUAL_DISK_ACCESS_NONE, NULL,
                          CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &p, NULL, &h);
    if (r != ERROR_SUCCESS) fatal(L"cannot create base disk %s (error %lu)", path, r);
    CloseHandle(h);
}

/* The parent the disk's own file records, 0 or a Win32 error. */
static DWORD disk_parent(const wchar_t *path, wchar_t *out, size_t out_len)
{
    VIRTUAL_STORAGE_TYPE st;
    OPEN_VIRTUAL_DISK_PARAMETERS p;
    HANDLE h = INVALID_HANDLE_VALUE;
    BYTE buf[4096];
    GET_VIRTUAL_DISK_INFO *info = (GET_VIRTUAL_DISK_INFO *)buf;
    ULONG size = sizeof(buf);
    DWORD r;

    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    st.VendorId = VENDOR_MS;
    ZeroMemory(&p, sizeof(p));
    p.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    r = OpenVirtualDisk(&st, path, VIRTUAL_DISK_ACCESS_NONE, OPEN_VIRTUAL_DISK_FLAG_NONE, &p, &h);
    if (r != ERROR_SUCCESS) return r;
    ZeroMemory(buf, sizeof(buf));
    info->Version = GET_VIRTUAL_DISK_INFO_PARENT_LOCATION;
    r = GetVirtualDiskInformation(h, &size, info, NULL);
    CloseHandle(h);
    if (r != ERROR_SUCCESS) return r;
    wcscpy_s(out, out_len, info->ParentLocation.ParentLocationBuffer);
    return ERROR_SUCCESS;
}

static void marker_text(int k, char *out, size_t out_len)
{
    sprintf_s(out, out_len, "asb-marker-%d", k);
}

static ULONGLONG marker_offset(int k)
{
    return (k + 1) * MARKER_STEP;
}

/* The markers visible through a disk. 0 or a Win32 error from the helper. */
static DWORD visible_mask(const wchar_t *disk, int markers, unsigned *out, HelperReply *rep)
{
    ULONGLONG offset[HELPER_MAX_RECORDS];
    DWORD r;
    int i;

    *out = 0;
    if (markers == 0) return disk_try_attach(disk, TRUE, rep);
    for (i = 0; i < markers; i++) offset[i] = marker_offset(i);
    r = disk_read_many(disk, offset, markers, rep);
    if (r != ERROR_SUCCESS) return r;
    for (i = 0; i < markers; i++) {
        char want[HELPER_RECORD_SIZE];
        marker_text(i, want, sizeof(want));
        if (strcmp((const char *)rep->data[i], want) == 0) *out |= 1u << i;
    }
    return ERROR_SUCCESS;
}

/* ---- The simulated VM record: vms.cfg's VhdxPath, saved atomically as save_vm_list does ---- */

static BOOL vm_save(const World *w, const wchar_t *path)
{
    wchar_t temp[MAX_PATH];
    FILE *f;
    swprintf_s(temp, MAX_PATH, L"%s.new", w->vm_file);
    if (_wfopen_s(&f, temp, L"w, ccs=UTF-8") != 0 || !f) return FALSE;
    fwprintf(f, L"%s\n", path);
    fclose(f);
    return MoveFileExW(temp, w->vm_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static BOOL vm_load(const World *w, wchar_t *out)
{
    FILE *f;
    size_t len;
    out[0] = L'\0';
    if (_wfopen_s(&f, w->vm_file, L"r, ccs=UTF-8") != 0 || !f) return FALSE;
    if (!fgetws(out, MAX_PATH, f)) out[0] = L'\0';
    fclose(f);
    len = wcslen(out);
    while (len > 0 && (out[len - 1] == L'\n' || out[len - 1] == L'\r')) out[--len] = L'\0';
    return len > 0;
}

/* ---- Building a world through the code under test, with no faults ---- */

static void world_open(World *w, SnapshotTree **t, VmInstance **v)
{
    *t = (SnapshotTree *)calloc(1, sizeof(SnapshotTree));
    *v = (VmInstance *)calloc(1, sizeof(VmInstance));
    if (!*t || !*v) fatal(L"out of memory");
    snapshot_init(*t, w->snap_dir);
    if (!vm_load(w, (*v)->vhdx_path)) fatal(L"cannot read the VM record in %s", w->dir);
}

static void world_close(World *w, SnapshotTree *t, VmInstance *v, BOOL save)
{
    if (save && !vm_save(w, v->vhdx_path)) fatal(L"cannot save the VM record in %s", w->dir);
    free(t);
    free(v);
}

static void world_create(World *w, const wchar_t *dir)
{
    ZeroMemory(w, sizeof(*w));
    wcscpy_s(w->dir, MAX_PATH, dir);
    swprintf_s(w->snap_dir, MAX_PATH, L"%s\\snapshots", dir);
    swprintf_s(w->base, MAX_PATH, L"%s\\disk.vhdx", dir);
    swprintf_s(w->vm_file, MAX_PATH, L"%s\\vm.txt", dir);
    CreateDirectoryW(dir, NULL);
    make_base(w->base);
    if (!vm_save(w, w->base)) fatal(L"cannot save the VM record in %s", dir);
}

/* Writes the next marker through the VM's current disk. */
static void world_mark(World *w)
{
    wchar_t path[MAX_PATH];
    char text[HELPER_RECORD_SIZE];
    HelperReply rep;
    DWORD r;

    if (!vm_load(w, path)) fatal(L"cannot read the VM record in %s", w->dir);
    marker_text(w->markers, text, sizeof(text));
    r = disk_write(path, marker_offset(w->markers), text, &rep);
    if (r != ERROR_SUCCESS)
        fatal(L"setup: cannot write through %s: %s (stage %lu)", path, rep.text, rep.stage);
    w->markers++;
}

static void world_take(World *w, const wchar_t *name)
{
    SnapshotTree *t;
    VmInstance *v;
    HRESULT hr;
    world_open(w, &t, &v);
    hr = snapshot_take(t, v, name);
    if (FAILED(hr)) fatal(L"setup: snapshot_take(%s) failed 0x%08X", name, hr);
    world_close(w, t, v, TRUE);
}

static void world_new_branch(World *w, int index)
{
    SnapshotTree *t;
    VmInstance *v;
    HRESULT hr;
    world_open(w, &t, &v);
    hr = snapshot_new_branch(t, v, index);
    if (FAILED(hr)) fatal(L"setup: snapshot_new_branch(%d) failed 0x%08X", index, hr);
    world_close(w, t, v, TRUE);
}

/* Points the VM straight at a frozen disk, as a crash before vms.cfg was saved can. */
static void world_on_snapshot_disk(World *w, int index)
{
    SnapshotTree *t;
    VmInstance *v;
    world_open(w, &t, &v);
    if (index >= t->count) fatal(L"setup: no snapshot %d", index);
    wcscpy_s(v->vhdx_path, MAX_PATH, index >= 0 ? t->nodes[index].snap_vhdx : t->base_vhdx);
    world_close(w, t, v, TRUE);
}

/* ---- Scenarios ---- */

static void setup_first(World *w)
{
    world_mark(w);
}

static void setup_on_snapshot_branch(World *w)
{
    world_mark(w);
    world_take(w, L"s1");
    world_mark(w);
}

static void setup_on_base_branch(World *w)
{
    world_mark(w);
    world_take(w, L"s1");
    world_new_branch(w, -2);
    world_mark(w);
}

static void setup_nested(World *w)
{
    world_mark(w);
    world_take(w, L"s1");
    world_mark(w);
    world_take(w, L"s2");
    world_mark(w);
    world_take(w, L"s3");
    world_mark(w);
}

static void setup_on_frozen_snapshot(World *w)
{
    world_mark(w);
    world_take(w, L"s1");
    world_mark(w);
    world_take(w, L"s2");
    world_on_snapshot_disk(w, 1);
}

static void setup_on_frozen_base(World *w)
{
    world_mark(w);
    world_take(w, L"s1");
    world_on_snapshot_disk(w, -1);
}

static void setup_two_branches(World *w)
{
    world_mark(w);
    world_take(w, L"s1");
    world_mark(w);
    world_new_branch(w, 0);
    world_mark(w);
}

static const Scenario SCENARIOS[] = {
    { L"first-snapshot",        setup_first,              OP_DO_TAKE,          0, 0 },
    { L"take-on-base-branch",   setup_on_base_branch,     OP_DO_TAKE,          0, 0 },
    { L"take-on-snap-branch",   setup_on_snapshot_branch, OP_DO_TAKE,          0, 0 },
    { L"take-nested",           setup_nested,             OP_DO_TAKE,          0, 0 },
    { L"take-on-frozen-snap",   setup_on_frozen_snapshot, OP_DO_TAKE,          0, 0 },
    { L"take-on-frozen-base",   setup_on_frozen_base,     OP_DO_TAKE,          0, 0 },
    { L"new-branch",            setup_on_snapshot_branch, OP_DO_NEW_BRANCH,    0, 0 },
    { L"start-on-frozen-snap",  setup_on_frozen_snapshot, OP_DO_START,         0, 0 },
    { L"start-on-frozen-base",  setup_on_frozen_base,     OP_DO_START,         0, 0 },
    { L"delete-other-branch",   setup_two_branches,       OP_DO_DELETE_BRANCH, 0, 0 },
    { L"delete-current-branch", setup_two_branches,       OP_DO_DELETE_BRANCH, 0, 1 },
    { L"delete-snapshot",       setup_nested,             OP_DO_DELETE,        2, 0 },
    { L"delete-last-snapshot",  setup_on_snapshot_branch, OP_DO_DELETE,        0, 0 },
};
#define SCENARIO_COUNT (sizeof(SCENARIOS) / sizeof(SCENARIOS[0]))

/* ---- The operation under test, in its own process so it can really be killed ---- */

/* What delete_disks in asb_core.c does. A failed delete must leave the VM where the
   delete found it; if not, *vm_kept is cleared. */
static HRESULT delete_as_asb_core(const World *w, SnapshotTree *t, VmInstance *v, int index, int branch, BOOL *vm_kept)
{
    wchar_t previous[MAX_PATH], left_for[MAX_PATH];
    HRESULT hr;

    wcscpy_s(previous, MAX_PATH, v->vhdx_path);
    hr = snapshot_leave_disks(t, v, index, branch);
    if (hr == S_OK) {
        BOOL saved = FALSE;
        if (!fault_point("save_vm_list")) {
            saved = vm_save(w, v->vhdx_path);
            fault_point_done();
        }
        if (!saved) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        wcscpy_s(left_for, MAX_PATH, v->vhdx_path);
        hr = branch >= 0 ? snapshot_delete_branch(t, v, index, branch) : snapshot_delete(t, v, index);
        if (FAILED(hr) && wcscmp(left_for, v->vhdx_path) != 0) *vm_kept = FALSE;
    }
    if (FAILED(hr)) wcscpy_s(v->vhdx_path, MAX_PATH, previous);
    return hr;
}

static int child_main(int argc, wchar_t **argv)
{
    World w;
    SnapshotTree *t, *before;
    VmInstance *v;
    wchar_t path_before[MAX_PATH];
    int op, arg, arg2, site;
    FaultAction action;
    const wchar_t *trace;
    HRESULT hr = E_FAIL;
    BOOL clean;
    BOOL vm_saved = FALSE;
    BOOL vm_kept = TRUE;
    HANDLE tree_lock = INVALID_HANDLE_VALUE;

    /* child <dir> <fence> <op> <arg> <arg2> <site> <action> <trace or -> <lock tree.dat once loaded> */
    if (argc != 11) return 2;
    ZeroMemory(&w, sizeof(w));
    wcscpy_s(w.dir, MAX_PATH, argv[2]);
    swprintf_s(w.snap_dir, MAX_PATH, L"%s\\snapshots", w.dir);
    swprintf_s(w.vm_file, MAX_PATH, L"%s\\vm.txt", w.dir);
    fault_set_fence(argv[3]);
    op = _wtoi(argv[4]);
    arg = _wtoi(argv[5]);
    arg2 = _wtoi(argv[6]);
    site = _wtoi(argv[7]);
    action = (FaultAction)_wtoi(argv[8]);
    trace = wcscmp(argv[9], L"-") == 0 ? NULL : argv[9];

    world_open(&w, &t, &v);
    before = (SnapshotTree *)malloc(sizeof(SnapshotTree));
    if (!before) return 3;
    memcpy(before, t, sizeof(SnapshotTree));
    wcscpy_s(path_before, MAX_PATH, v->vhdx_path);

    if (_wtoi(argv[10])) {
        wchar_t tree_path[MAX_PATH];
        swprintf_s(tree_path, MAX_PATH, L"%s\\tree.dat", w.snap_dir);
        tree_lock = CreateFileW(tree_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }

    fault_arm(site, action, trace);
    switch (op) {
    case OP_DO_TAKE:          hr = snapshot_take(t, v, L"target"); break;
    case OP_DO_NEW_BRANCH:    hr = snapshot_new_branch(t, v, arg); break;
    case OP_DO_START:         hr = snapshot_ensure_writable(t, v); break;
    case OP_DO_DELETE_BRANCH: hr = delete_as_asb_core(&w, t, v, arg, arg2, &vm_kept); break;
    case OP_DO_DELETE:        hr = delete_as_asb_core(&w, t, v, arg, -1, &vm_kept); break;
    }
    /* asb_snap_take and friends save vms.cfg whatever happened */
    if (!fault_point("save_vm_list")) {
        vm_saved = vm_save(&w, v->vhdx_path);
        fault_point_done();
    }
    fault_disarm();
    if (tree_lock != INVALID_HANDLE_VALUE) CloseHandle(tree_lock);

    if (SUCCEEDED(hr)) return vm_saved ? 0 : EXIT_DONE_VM_UNSAVED;
    clean = vm_kept && memcmp(before, t, sizeof(SnapshotTree)) == 0 && wcscmp(path_before, v->vhdx_path) == 0;
    if (!clean) return EXIT_FAILED_DIRTY;
    return vm_saved ? EXIT_FAILED_CLEAN : EXIT_FAILED_VM_UNSAVED;
}

static DWORD spawn_child(const World *w, const Scenario *s, int site, FaultAction action, const wchar_t *trace,
                         BOOL lock_tree_once_loaded)
{
    wchar_t cmd[MAX_PATH * 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    swprintf_s(cmd, MAX_PATH * 4, L"\"%s\" child \"%s\" \"%s\" %d %d %d %d %d \"%s\" %d",
               g_exe, w->dir, g_run_dir, s->op, s->arg, s->arg2, site, (int)action, trace ? trace : L"-",
               lock_tree_once_loaded ? 1 : 0);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(g_exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        fatal(L"cannot start the child process (error %lu)", GetLastError());
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        fatal(L"the operation hung in %s", w->dir);
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

/* ---- What the metadata says ---- */

static int named_disks(const SnapshotTree *t, Named *out)
{
    int n = 0, i, b;

    if (t->base_vhdx[0]) {
        wcscpy_s(out[n].path, MAX_PATH, t->base_vhdx);
        out[n].is_branch = FALSE;
        out[n].snap = -2;
        n++;
    }
    for (b = 0; b < t->base_branch_count && n < MAX_NAMED; b++) {
        if (!t->base_branches[b].valid) continue;
        wcscpy_s(out[n].path, MAX_PATH, t->base_branches[b].vhdx_path);
        out[n].is_branch = TRUE;
        out[n].snap = -2;
        n++;
    }
    for (i = 0; i < t->count && n < MAX_NAMED; i++) {
        if (!t->nodes[i].valid) continue;
        wcscpy_s(out[n].path, MAX_PATH, t->nodes[i].snap_vhdx);
        out[n].is_branch = FALSE;
        out[n].snap = i;
        n++;
        for (b = 0; b < t->nodes[i].branch_count && n < MAX_NAMED; b++) {
            if (!t->nodes[i].branches[b].valid) continue;
            wcscpy_s(out[n].path, MAX_PATH, t->nodes[i].branches[b].vhdx_path);
            out[n].is_branch = TRUE;
            out[n].snap = i;
            n++;
        }
    }
    return n;
}

static BOOL is_named(const Named *named, int n, const wchar_t *path)
{
    int i;
    for (i = 0; i < n; i++)
        if (same_path(named[i].path, path)) return TRUE;
    return FALSE;
}

/* The Parent= of the snapshot with this GUID, as tree.dat has it on disk. Read as text
   because not every version under test has the field in its structures. */
static void tree_parent_of(const World *w, const wchar_t *guid, wchar_t *out, size_t out_len)
{
    wchar_t path[MAX_PATH], line[1024];
    FILE *f;
    BOOL in_snapshot = FALSE, this_one = FALSE;

    out[0] = L'\0';
    swprintf_s(path, MAX_PATH, L"%s\\tree.dat", w->snap_dir);
    if (_wfopen_s(&f, path, L"r, ccs=UTF-8") != 0 || !f) return;
    while (fgetws(line, 1024, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) line[--len] = L'\0';
        if (line[0] == L'[') {
            in_snapshot = wcscmp(line, L"[Snapshot]") == 0;
            this_one = FALSE;
        } else if (in_snapshot && wcsncmp(line, L"Guid=", 5) == 0) {
            this_one = _wcsicmp(line + 5, guid) == 0;
        } else if (in_snapshot && this_one && wcsncmp(line, L"Parent=", 7) == 0) {
            wcscpy_s(out, out_len, line + 7);
        }
    }
    fclose(f);
}

static int list_disk_files(const World *w, wchar_t files[][MAX_PATH], int cap)
{
    const wchar_t *dirs[2];
    int n = 0, d;

    dirs[0] = w->dir;
    dirs[1] = w->snap_dir;
    for (d = 0; d < 2; d++) {
        wchar_t pattern[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        swprintf_s(pattern, MAX_PATH, L"%s\\*.vhdx", dirs[d]);
        h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (n < cap) {
                swprintf_s(files[n], MAX_PATH, L"%s\\%s", dirs[d], fd.cFileName);
                n++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

/* ---- Before the operation ---- */

static BOOL is_doomed(const Pre *pre, const wchar_t *path)
{
    int i;
    for (i = 0; i < pre->ndoomed; i++)
        if (same_path(pre->doomed[i], path)) return TRUE;
    return FALSE;
}

static void capture_pre(const World *w, const Scenario *s, Pre *pre)
{
    SnapshotTree *t = (SnapshotTree *)calloc(1, sizeof(SnapshotTree));
    HelperReply rep;
    int i;

    if (!t) fatal(L"out of memory");
    ZeroMemory(pre, sizeof(*pre));
    snapshot_init(t, w->snap_dir);
    if (!vm_load(w, pre->active)) fatal(L"cannot read the VM record");

    if (visible_mask(pre->active, w->markers, &pre->visible, &rep) != ERROR_SUCCESS)
        fatal(L"setup: cannot read %s: %s", pre->active, rep.text);
    pre->visible_target = pre->visible;
    if (s->op == OP_DO_NEW_BRANCH) {
        const wchar_t *target = s->arg == -2 ? t->base_vhdx : t->nodes[s->arg].snap_vhdx;
        if (visible_mask(target, w->markers, &pre->visible_target, &rep) != ERROR_SUCCESS)
            fatal(L"setup: cannot read %s: %s", target, rep.text);
    }

    pre->nfiles = list_disk_files(w, pre->files, MAX_FILES);
    for (i = 0; i < pre->nfiles; i++)
        if (!hash_file(pre->files[i], pre->hash[i])) fatal(L"setup: cannot hash %s", pre->files[i]);

    pre->nnamed = named_disks(t, pre->named);
    pre->had_base = t->base_vhdx[0] != L'\0';
    for (i = 0; i < t->count; i++) {
        if (t->nodes[i].valid)
            wcscpy_s(pre->snap_names[pre->nsnaps++], 128, t->nodes[i].name);
    }

    pre->must_survive = (1u << w->markers) - 1;
    if (s->op == OP_DO_DELETE_BRANCH) {
        wcscpy_s(pre->doomed[pre->ndoomed++], MAX_PATH, t->nodes[s->arg].branches[s->arg2].vhdx_path);
    } else if (s->op == OP_DO_DELETE) {
        const SnapNode *node = &t->nodes[s->arg];
        wcscpy_s(pre->doomed_snapshot, 128, node->name);
        wcscpy_s(pre->doomed[pre->ndoomed++], MAX_PATH, node->snap_vhdx);
        for (i = 0; i < node->branch_count; i++)
            wcscpy_s(pre->doomed[pre->ndoomed++], MAX_PATH, node->branches[i].vhdx_path);
    }
    if (pre->ndoomed > 0) {
        /* A delete really does lose what only the deleted disks held. */
        pre->must_survive = 0;
        for (i = 0; i < pre->nnamed; i++) {
            unsigned seen = 0;
            if (is_doomed(pre, pre->named[i].path)) continue;
            if (visible_mask(pre->named[i].path, w->markers, &seen, &rep) != ERROR_SUCCESS)
                fatal(L"setup: cannot read %s: %s", pre->named[i].path, rep.text);
            pre->must_survive |= seen;
        }
    }
    free(t);
}

/* ---- After the operation: what a restart finds ---- */

static void verify_chain(const World *w, const SnapshotTree *t, Result *r)
{
    Named named[MAX_NAMED];
    int n = named_disks(t, named);
    HelperReply rep;
    int i, j;

    for (i = 0; i < n; i++) {
        wchar_t parent[MAX_PATH], want[MAX_PATH];
        DWORD e;

        want[0] = L'\0';
        if (same_path(named[i].path, t->base_vhdx)) continue;
        if (!exists(named[i].path)) {
            note(r, CHECK_CHAIN, L"%s does not exist", leaf(named[i].path));
            continue;
        }
        e = disk_parent(named[i].path, parent, MAX_PATH);
        if (e != ERROR_SUCCESS) {
            note(r, CHECK_CHAIN, L"%s will not open (error %lu)", leaf(named[i].path), e);
            continue;
        }
        if (named[i].is_branch) {
            wcscpy_s(want, MAX_PATH, named[i].snap == -2 ? t->base_vhdx : t->nodes[named[i].snap].snap_vhdx);
        } else {
            wchar_t guid[64];
            tree_parent_of(w, t->nodes[named[i].snap].guid, guid, 64);
            wcscpy_s(want, MAX_PATH, t->base_vhdx);
            if (guid[0]) {
                BOOL found = FALSE;
                for (j = 0; j < t->count; j++) {
                    if (t->nodes[j].valid && _wcsicmp(t->nodes[j].guid, guid) == 0) {
                        wcscpy_s(want, MAX_PATH, t->nodes[j].snap_vhdx);
                        found = TRUE;
                    }
                }
                if (!found) note(r, CHECK_CHAIN, L"Parent=%s names no snapshot", guid);
            }
        }
        if (!same_path(parent, want))
            note(r, CHECK_CHAIN, L"%s has parent %s, metadata says %s", leaf(named[i].path), leaf(parent), leaf(want));

        /* A working branch can be written, so nothing may depend on it. */
        for (j = 0; j < n; j++) {
            if (named[j].is_branch && same_path(parent, named[j].path))
                note(r, CHECK_CHAIN, L"%s has a working branch as its parent: %s", leaf(named[i].path), leaf(parent));
        }
    }
    /* Attaching a branch opens the whole chain under it. */
    for (i = 0; i < n; i++) {
        BOOL leaf_disk = named[i].is_branch;
        if (!leaf_disk && named[i].snap >= 0) leaf_disk = t->nodes[named[i].snap].branch_count == 0;
        if (!leaf_disk || !exists(named[i].path)) continue;
        if (disk_try_attach(named[i].path, TRUE, &rep) != ERROR_SUCCESS)
            note(r, CHECK_CHAIN, L"%s will not attach: %s", leaf(named[i].path), rep.text);
    }
}

static void verify(World *w, const Scenario *s, const Pre *pre, DWORD code, Result *r)
{
    SnapshotTree *t = (SnapshotTree *)calloc(1, sizeof(SnapshotTree));
    VmInstance *v = (VmInstance *)calloc(1, sizeof(VmInstance));
    Named named[MAX_NAMED];
    int nnamed, i, cur_snap = -1, cur_branch = -1;
    wchar_t recorded[MAX_PATH], active[MAX_PATH];
    HelperReply rep;
    BOOL have_active = TRUE;
    BOOL reported_failure = code == EXIT_FAILED_CLEAN || code == EXIT_FAILED_DIRTY || code == EXIT_FAILED_VM_UNSAVED;
    HRESULT hr;

    if (!t || !v) fatal(L"out of memory");

    /* The operation itself must not change, move or delete any existing disk, apart
       from those a delete is there to remove, if it went ahead. */
    for (i = 0; i < pre->nfiles; i++) {
        BYTE now[32];
        if (!exists(pre->files[i])) {
            if (is_doomed(pre, pre->files[i]) && !reported_failure) continue;
            note(r, CHECK_NO_RENAME, L"%s is gone", leaf(pre->files[i]));
        } else if (hash_file(pre->files[i], now) && memcmp(now, pre->hash[i], 32) != 0) {
            note(r, CHECK_IMMUTABLE, L"the operation changed the bytes of %s", leaf(pre->files[i]));
        }
    }

    /* The restart */
    if (!vm_load(w, recorded)) {
        note(r, CHECK_METADATA, L"the VM record is unreadable");
        recorded[0] = L'\0';
    }
    snapshot_init(t, w->snap_dir);
    wcscpy_s(v->vhdx_path, MAX_PATH, recorded);
    nnamed = named_disks(t, named);

    /* What the operation claimed against what is there */
    if (code == EXIT_FAILED_DIRTY)
        note(r, CHECK_HONEST, L"it reported failure, but left the tree or the VM changed in memory");
    if (reported_failure) {
        /* Where saving vms.cfg failed too, its record may lag by a disk that still exists. */
        if (code != EXIT_FAILED_VM_UNSAVED && !same_path(recorded, pre->active))
            note(r, CHECK_HONEST, L"it reported failure, but the VM moved to %s", leaf(recorded));
        if (t->count != pre->nsnaps)
            note(r, CHECK_HONEST, L"it reported failure, but the tree has %d snapshots, was %d", t->count, pre->nsnaps);
        for (i = 0; i < pre->ndoomed; i++) {
            if (!is_named(named, nnamed, pre->doomed[i]))
                note(r, CHECK_HONEST, L"it reported failure, but tree.dat no longer names %s", leaf(pre->doomed[i]));
        }
    }
    /* If saving vms.cfg failed afterwards, its VM record lags: the next start deals with
       that, so it is not the operation's report that was wrong. */
    if (code == 0 || code == EXIT_DONE_VM_UNSAVED) {
        for (i = 0; i < pre->ndoomed; i++) {
            if (is_named(named, nnamed, pre->doomed[i]))
                note(r, CHECK_HONEST, L"it reported success, but tree.dat still names %s", leaf(pre->doomed[i]));
        }
    }
    if ((code == 0 || code == EXIT_DONE_VM_UNSAVED) && s->op == OP_DO_TAKE) {
        BOOL found = FALSE;
        snapshot_find_current(t, recorded, &cur_snap, &cur_branch);
        for (i = 0; i < t->count; i++) {
            if (t->nodes[i].valid && wcscmp(t->nodes[i].name, L"target") == 0) {
                found = TRUE;
                if (code == 0 && (i != cur_snap || cur_branch < 0))
                    note(r, CHECK_HONEST, L"it reported success, but the VM is not on the new snapshot's branch");
            }
        }
        if (!found) note(r, CHECK_HONEST, L"it reported success, but the new snapshot is not in tree.dat");
    }
    /* In these scenarios the VM starts out on a frozen disk, so it must have moved. */
    if (code == 0 && s->op == OP_DO_START && same_path(recorded, pre->active))
        note(r, CHECK_IMMUTABLE, L"the start left the VM to run on the frozen disk %s", leaf(recorded));
    if (code == 0 && (s->op == OP_DO_NEW_BRANCH || s->op == OP_DO_START)) {
        snapshot_find_current(t, recorded, &cur_snap, &cur_branch);
        if (cur_branch < 0)
            note(r, CHECK_HONEST, L"it reported success, but tree.dat does not list the VM's branch");
    }

    /* The metadata as a restart reads it */
    for (i = 0; i < nnamed; i++) {
        if (!exists(named[i].path))
            note(r, CHECK_METADATA, L"tree.dat names %s, which does not exist", leaf(named[i].path));
    }
    /* Deleting the last snapshot does forget the base, on purpose. */
    if (pre->had_base && !t->base_vhdx[0] && !(pre->ndoomed > 0 && pre->nsnaps == 1 && pre->doomed_snapshot[0]))
        note(r, CHECK_METADATA, L"tree.dat has lost the base disk");
    for (i = 0; i < pre->nsnaps; i++) {
        int j;
        BOOL found = FALSE;
        if (wcscmp(pre->snap_names[i], pre->doomed_snapshot) == 0) continue;
        for (j = 0; j < t->count; j++)
            if (t->nodes[j].valid && wcscmp(t->nodes[j].name, pre->snap_names[i]) == 0) found = TRUE;
        if (!found) note(r, CHECK_METADATA, L"snapshot %s is no longer listed", pre->snap_names[i]);
    }
    for (i = 0; i < pre->nnamed; i++) {
        if (is_doomed(pre, pre->named[i].path)) continue;
        if (nnamed == 0 && pre->nsnaps == 1 && pre->doomed_snapshot[0]) continue;
        if (!is_named(named, nnamed, pre->named[i].path))
            note(r, CHECK_METADATA, L"tree.dat no longer names %s", leaf(pre->named[i].path));
    }

    /* The next start: a frozen disk gets a branch first */
    hr = snapshot_ensure_writable(t, v);
    if (FAILED(hr)) {
        note(r, CHECK_METADATA, L"the next start fails: 0x%08X", hr);
        have_active = FALSE;
    } else if (hr == S_OK) {
        vm_save(w, v->vhdx_path);
    }
    wcscpy_s(active, MAX_PATH, v->vhdx_path);
    nnamed = named_disks(t, named);

    /* Judged after the start, since a VM record that lags is what the start is there to fix */
    if (!active[0] || !exists(active)) {
        note(r, CHECK_METADATA, L"the VM record names %s, which does not exist", leaf(active));
        have_active = FALSE;
    } else if (nnamed > 0 && !is_named(named, nnamed, active)) {
        note(r, CHECK_METADATA, L"the VM is on %s, which tree.dat does not know", leaf(active));
    }

    /* Content: what the VM will see */
    if (!have_active) {
        note(r, CHECK_CONTENT, L"the VM has no disk it can start on");
        note(r, CHECK_USABLE, L"the VM has no disk it can start on");
    } else {
        unsigned seen = 0;
        unsigned expected = pre->visible;
        BOOL moved = !same_path(active, pre->active);
        /* A delete that took the VM's own disk puts it somewhere else on purpose. */
        BOOL any_content = moved && pre->ndoomed > 0;
        if (s->op == OP_DO_NEW_BRANCH && moved)
            expected = pre->visible_target;
        if (visible_mask(active, w->markers, &seen, &rep) != ERROR_SUCCESS) {
            note(r, CHECK_CONTENT, L"cannot read the VM's disk %s: %s", leaf(active), rep.text);
        } else if (!any_content && seen != expected) {
            wchar_t got[128], want[128];
            mask_text(seen, got, 128);
            mask_text(expected, want, 128);
            note(r, CHECK_CONTENT, L"the VM's disk shows %s, should show %s", got, want);
        }
    }

    /* No loss: everything written is reachable from something the metadata names */
    {
        unsigned found = 0;
        if (have_active) {
            unsigned seen = 0;
            if (visible_mask(active, w->markers, &seen, &rep) == ERROR_SUCCESS) found |= seen;
        }
        for (i = nnamed - 1; i >= 0 && (pre->must_survive & ~found) != 0; i--) {
            unsigned seen = 0;
            if (exists(named[i].path) && visible_mask(named[i].path, w->markers, &seen, &rep) == ERROR_SUCCESS)
                found |= seen;
        }
        if ((pre->must_survive & ~found) != 0) {
            wchar_t missing[128];
            mask_text(pre->must_survive & ~found, missing, 128);
            note(r, CHECK_NO_LOSS, L"nothing the metadata names shows %s", missing);
        }
    }

    verify_chain(w, t, r);

    /* The VM keeps working: frozen disks stay put while it writes */
    if (have_active) {
        BYTE before[MAX_NAMED][32];
        BOOL hashed[MAX_NAMED];
        char text[HELPER_RECORD_SIZE];
        int k = w->markers;

        for (i = 0; i < nnamed; i++) {
            if (!named[i].is_branch && same_path(named[i].path, active))
                note(r, CHECK_IMMUTABLE, L"the VM would run on the frozen disk %s", leaf(active));
            hashed[i] = !named[i].is_branch && hash_file(named[i].path, before[i]);
        }

        marker_text(k, text, sizeof(text));
        if (disk_write(active, marker_offset(k), text, &rep) != ERROR_SUCCESS) {
            note(r, CHECK_USABLE, L"cannot write through %s: %s", leaf(active), rep.text);
        } else {
            char back[HELPER_RECORD_SIZE];
            if (disk_read(active, marker_offset(k), back, sizeof(back), &rep) != ERROR_SUCCESS || strcmp(back, text) != 0)
                note(r, CHECK_USABLE, L"what was written to %s did not read back", leaf(active));
            for (i = 0; i < nnamed; i++) {
                BYTE after[32];
                if (!hashed[i]) continue;
                if (hash_file(named[i].path, after) && memcmp(before[i], after, 32) != 0)
                    note(r, CHECK_IMMUTABLE, L"writing through the VM's disk changed the frozen %s", leaf(named[i].path));
                if (disk_read(named[i].path, marker_offset(k), back, sizeof(back), &rep) == ERROR_SUCCESS &&
                    strcmp(back, text) == 0)
                    note(r, CHECK_IMMUTABLE, L"the frozen %s shows data written after it was frozen", leaf(named[i].path));
            }
        }

        /* The next snapshot, with nothing going wrong */
        hr = snapshot_take(t, v, L"after");
        if (FAILED(hr)) {
            note(r, CHECK_USABLE, L"the next snapshot fails: 0x%08X, at %S", hr, fault_last_op());
        } else {
            vm_save(w, v->vhdx_path);
            if (disk_try_attach(v->vhdx_path, FALSE, &rep) != ERROR_SUCCESS)
                note(r, CHECK_USABLE, L"the VM's disk after the next snapshot will not attach: %s", rep.text);
        }
    }

    free(t);
    free(v);
}

/* ---- Running cases ---- */

static void remove_tree(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (_wcsnicmp(dir, g_run_dir, wcslen(g_run_dir)) != 0 || dir[wcslen(g_run_dir)] != L'\\')
        fatal(L"refusing to remove %s", dir);
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t path[MAX_PATH];
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            swprintf_s(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(path, fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(path);
            } else {
                int tries;
                for (tries = 0; tries < 40 && !DeleteFileW(path); tries++) Sleep(50);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

typedef enum {
    VARIANT_NONE,
    VARIANT_DISK_LOCKED,
    VARIANT_TREE_READONLY,
    VARIANT_TREE_LOCKED_AT_LOAD,
    VARIANT_TREE_LOCKED_AT_SAVE
} Variant;

static const wchar_t *action_name(FaultAction a)
{
    switch (a) {
    case FAULT_FAIL:        return L"fail";
    case FAULT_FAIL_ALL:    return L"fail-all-after";
    case FAULT_KILL_BEFORE: return L"kill-before";
    case FAULT_KILL_AFTER:  return L"kill-after";
    default:                return L"none";
    }
}

static const wchar_t *outcome_name(DWORD code)
{
    switch (code) {
    case 0:
    case EXIT_DONE_VM_UNSAVED: return L"reported-success";
    case EXIT_FAILED_CLEAN:
    case EXIT_FAILED_DIRTY:
    case EXIT_FAILED_VM_UNSAVED: return L"reported-failure";
    case EXIT_KILLED:       return L"killed";
    default:                return L"?";
    }
}

/* Every attach is a device arrival, and this many of them hang the Windows shell: the
   taskbar stops responding, or explorer dies and the taskbar is gone. A run stops adding
   to that until the shell is back, which does not keep the desktop usable: see README. */
static BOOL shell_is_down(void)
{
    static BOOL seen;
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);

    /* Some machines have no taskbar at all, so gone counts only if there was one */
    if (!tray) return seen;
    seen = TRUE;
    return IsHungAppWindow(tray);
}

static void wait_for_shell(void)
{
    ULONGLONG started;

    if (!shell_is_down()) return;
    started = GetTickCount64();
    wprintf(L"    the Windows taskbar is hung or gone: pausing until it recovers\n");
    fflush(stdout);
    g_shell_pauses++;
    while (shell_is_down()) {
        if (GetTickCount64() - started > 30ULL * 60 * 1000)
            fatal(L"the Windows taskbar has not recovered in 30 minutes");
        Sleep(2000);
    }
    g_shell_paused_ms += GetTickCount64() - started;
}

static void run_case(const Scenario *s, const wchar_t *label, int site, FaultAction action,
                     Variant variant, const wchar_t *trace)
{
    wchar_t dir[MAX_PATH], scenario_dir[MAX_PATH], tree_path[MAX_PATH];
    World w;
    static Pre pre;
    static Result r;
    HANDLE lock = INVALID_HANDLE_VALUE;
    DWORD code;
    BOOL any = FALSE;
    int i;

    wait_for_shell();
    swprintf_s(scenario_dir, MAX_PATH, L"%s\\%s", g_run_dir, s->name);
    CreateDirectoryW(scenario_dir, NULL);
    swprintf_s(dir, MAX_PATH, L"%s\\%d", scenario_dir, g_cases);
    world_create(&w, dir);
    s->setup(&w);
    capture_pre(&w, s, &pre);
    swprintf_s(tree_path, MAX_PATH, L"%s\\tree.dat", w.snap_dir);

    if (variant == VARIANT_DISK_LOCKED) {
        lock = CreateFileW(pre.active, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (lock == INVALID_HANDLE_VALUE) fatal(L"cannot lock %s (error %lu)", pre.active, GetLastError());
    } else if (variant == VARIANT_TREE_LOCKED_AT_LOAD && exists(tree_path)) {
        lock = CreateFileW(tree_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (lock == INVALID_HANDLE_VALUE) fatal(L"cannot lock tree.dat (error %lu)", GetLastError());
    } else if (variant == VARIANT_TREE_READONLY && exists(tree_path)) {
        SetFileAttributesW(tree_path, FILE_ATTRIBUTE_READONLY);
    }

    code = spawn_child(&w, s, site, action, trace, variant == VARIANT_TREE_LOCKED_AT_SAVE);

    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    if (variant == VARIANT_TREE_READONLY && exists(tree_path))
        SetFileAttributesW(tree_path, FILE_ATTRIBUTE_NORMAL);

    if (code != 0 && code != EXIT_DONE_VM_UNSAVED && code != EXIT_FAILED_CLEAN &&
        code != EXIT_FAILED_DIRTY && code != EXIT_FAILED_VM_UNSAVED && code != EXIT_KILLED)
        fatal(L"%s/%s: the operation ended with exit code %lu", s->name, label, code);

    ZeroMemory(&r, sizeof(r));
    verify(&w, s, &pre, code, &r);

    g_cases++;
    fwprintf(g_tsv, L"%s\t%s\t%s", s->name, label, outcome_name(code));
    for (i = 0; i < CHECK_COUNT; i++) {
        fwprintf(g_tsv, L"\t%s", r.failed[i] ? L"FAIL" : L"ok");
        if (r.failed[i]) {
            g_check_failed[i]++;
            any = TRUE;
        }
    }
    for (i = 0; i < CHECK_COUNT; i++)
        if (r.failed[i]) fwprintf(g_tsv, L"\t%s: %s", CHECK_NAME[i], r.why[i]);
    fwprintf(g_tsv, L"\n");
    fflush(g_tsv);
    if (any) g_cases_failed++;

    if (!g_keep) remove_tree(dir);
}

/* ---- The sites to attack: what a scenario really does, recorded ---- */

typedef struct {
    int     ordinal;
    wchar_t name[64];
} Site;

static int load_trace(const wchar_t *path, Site *sites, int cap)
{
    FILE *f;
    wchar_t name[64];
    int ordinal, n = 0;

    if (_wfopen_s(&f, path, L"r") != 0 || !f) fatal(L"no trace at %s", path);
    while (fwscanf_s(f, L"%d %63s", &ordinal, name, 64) == 2) {
        /* One long run of writes: the first and the last will do. */
        if (n >= 2 && wcscmp(name, L"fwprintf") == 0 &&
            wcscmp(sites[n - 1].name, L"fwprintf") == 0 && wcscmp(sites[n - 2].name, L"fwprintf") == 0)
            n--;
        if (n >= cap) fatal(L"too many operations in %s", path);
        sites[n].ordinal = ordinal;
        wcscpy_s(sites[n].name, 64, name);
        n++;
    }
    fclose(f);
    return n;
}

static BOOL wanted(const Scenario *s)
{
    return !g_filter || wcsstr(s->name, g_filter) != NULL;
}

static void run_matrix(void)
{
    static const FaultAction ACTIONS[] = { FAULT_FAIL, FAULT_FAIL_ALL, FAULT_KILL_BEFORE, FAULT_KILL_AFTER };
    static const struct { Variant v; const wchar_t *label; } VARIANTS[] = {
        { VARIANT_DISK_LOCKED,   L"VM-disk-locked-by-another-handle" },
        { VARIANT_TREE_READONLY, L"tree.dat-read-only" },
        { VARIANT_TREE_LOCKED_AT_LOAD, L"tree.dat-locked-when-loaded" },
        { VARIANT_TREE_LOCKED_AT_SAVE, L"tree.dat-locked-when-saved" },
    };
    static Site sites[SCENARIO_COUNT][MAX_SITES];
    int nsites[SCENARIO_COUNT];
    size_t s;
    int i, a, v;

    /* First pass: nothing goes wrong, and the trace says what there is to attack. */
    g_planned = 0;
    for (s = 0; s < SCENARIO_COUNT; s++) {
        wchar_t trace[MAX_PATH];
        nsites[s] = 0;
        if (!wanted(&SCENARIOS[s])) continue;
        swprintf_s(trace, MAX_PATH, L"%s\\%s.trace", g_run_dir, SCENARIOS[s].name);
        run_case(&SCENARIOS[s], L"no-fault", 0, FAULT_NONE, VARIANT_NONE, trace);
        nsites[s] = load_trace(trace, sites[s], MAX_SITES);
        g_planned += 1 + nsites[s] * 4 + 4;
    }
    say(L"Planned: %d cases.\n", g_planned);

    for (s = 0; s < SCENARIO_COUNT; s++) {
        if (!wanted(&SCENARIOS[s])) continue;
        say(L"  %-24s %d operations\n", SCENARIOS[s].name, nsites[s]);
        for (i = 0; i < nsites[s]; i++) {
            for (a = 0; a < 4; a++) {
                wchar_t label[128];
                swprintf_s(label, 128, L"%d-%s:%s", sites[s][i].ordinal, sites[s][i].name, action_name(ACTIONS[a]));
                run_case(&SCENARIOS[s], label, sites[s][i].ordinal, ACTIONS[a], VARIANT_NONE, NULL);
            }
        }
        for (v = 0; v < 4; v++)
            run_case(&SCENARIOS[s], VARIANTS[v].label, 0, FAULT_NONE, VARIANTS[v].v, NULL);
    }
}

/* ---- Behaviour with nothing going wrong ---- */

typedef void (*BehaviourTest)(const wchar_t *dir, wchar_t *why, size_t why_len);

/* Each snapshot holds exactly what the VM had when it was taken. */
static void test_snapshots_hold_their_state(const wchar_t *dir, wchar_t *why, size_t why_len)
{
    World w;
    SnapshotTree *t;
    VmInstance *v;
    HelperReply rep;
    int i;

    world_create(&w, dir);
    setup_nested(&w);
    world_open(&w, &t, &v);
    for (i = 0; i < t->count; i++) {
        unsigned seen = 0;
        unsigned want = (1u << (i + 1)) - 1;
        if (visible_mask(t->nodes[i].snap_vhdx, w.markers, &seen, &rep) != ERROR_SUCCESS) {
            swprintf_s(why, why_len, L"cannot read snapshot %s: %s", t->nodes[i].name, rep.text);
            break;
        }
        if (seen != want) {
            wchar_t got[128], should[128];
            mask_text(seen, got, 128);
            mask_text(want, should, 128);
            swprintf_s(why, why_len, L"snapshot %s shows %s, should show %s", t->nodes[i].name, got, should);
            break;
        }
    }
    world_close(&w, t, v, FALSE);
}

/* A tree.dat written before Parent= existed loads, and is written back byte for byte. */
static void test_old_tree_dat(const wchar_t *dir, wchar_t *why, size_t why_len)
{
    wchar_t snap_dir[MAX_PATH], path[MAX_PATH];
    SnapshotTree *t = (SnapshotTree *)calloc(1, sizeof(SnapshotTree));
    FILE *f;
    BYTE *before, *after;
    long nb = 0, na = 0;
    HRESULT hr;

    if (!t) fatal(L"out of memory");
    CreateDirectoryW(dir, NULL);
    swprintf_s(snap_dir, MAX_PATH, L"%s\\snapshots", dir);
    CreateDirectoryW(snap_dir, NULL);
    swprintf_s(path, MAX_PATH, L"%s\\tree.dat", snap_dir);
    if (_wfopen_s(&f, path, L"w, ccs=UTF-8") != 0 || !f) fatal(L"cannot write %s", path);
    fwprintf(f, L"[Base]\n%s\\disk.vhdx\n\n", dir);
    fwprintf(f, L"[BaseBranch]\nGuid=b0\nName=2026-03-21 14:30:00\nVhdx=%s\\branch_b0.vhdx\n\n", snap_dir);
    fwprintf(f, L"[Snapshot]\nGuid=s1\nName=old one\nVhdx=%s\\snapshot_s1.vhdx\nCreated=133500000000000000\n\n", snap_dir);
    fwprintf(f, L"[Branch]\nGuid=b1\nName=Default Branch\nVhdx=%s\\branch_b1.vhdx\n\n", snap_dir);
    fclose(f);

    before = (BYTE *)malloc(65536);
    after = (BYTE *)malloc(65536);
    if (!before || !after) fatal(L"out of memory");
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) fatal(L"cannot read %s", path);
    nb = (long)fread(before, 1, 65536, f);
    fclose(f);

    snapshot_init(t, snap_dir);
    if (t->count != 1 || t->base_branch_count != 1 || wcscmp(t->nodes[0].name, L"old one") != 0) {
        swprintf_s(why, why_len, L"loaded %d snapshots and %d base branches", t->count, t->base_branch_count);
    } else {
        hr = snapshot_rename(t, 0, -1, L"old one"); /* the same name: saves, changes nothing */
        if (FAILED(hr)) swprintf_s(why, why_len, L"saving failed 0x%08X", hr);
        if (_wfopen_s(&f, path, L"rb") != 0 || !f) fatal(L"cannot read %s", path);
        na = (long)fread(after, 1, 65536, f);
        fclose(f);
        if (!why[0] && (na != nb || memcmp(before, after, nb) != 0))
            swprintf_s(why, why_len, L"written back differently: %ld bytes, was %ld", na, nb);
    }
    free(before);
    free(after);
    free(t);
}

/* A snapshot another disk depends on cannot be deleted from under it. */
static void test_delete_guard(const wchar_t *dir, wchar_t *why, size_t why_len)
{
    World w;
    SnapshotTree *t;
    VmInstance *v;
    HelperReply rep;
    wchar_t parent[MAX_PATH], first[MAX_PATH];
    unsigned before = 0, after = 0;
    BOOL depends;
    HRESULT hr;

    world_create(&w, dir);
    setup_nested(&w);
    world_open(&w, &t, &v);
    wcscpy_s(first, MAX_PATH, t->nodes[0].snap_vhdx);
    if (visible_mask(v->vhdx_path, w.markers, &before, &rep) != ERROR_SUCCESS)
        fatal(L"cannot read %s", v->vhdx_path);
    depends = disk_parent(t->nodes[1].snap_vhdx, parent, MAX_PATH) == ERROR_SUCCESS && same_path(parent, first);

    hr = snapshot_delete(t, v, 0);
    if (depends && SUCCEEDED(hr)) {
        swprintf_s(why, why_len, L"deleted a snapshot another one depends on");
    } else if (depends && !exists(first)) {
        swprintf_s(why, why_len, L"refused, but the disk is gone");
    } else if (visible_mask(v->vhdx_path, w.markers, &after, &rep) != ERROR_SUCCESS) {
        swprintf_s(why, why_len, L"the VM's disk no longer reads: %s", rep.text);
    } else if (after != before) {
        swprintf_s(why, why_len, L"the VM's disk changed content");
    }
    world_close(&w, t, v, FALSE);
}

/* What a restart reads is what was in memory. */
static void test_restart_preserves_tree(const wchar_t *dir, wchar_t *why, size_t why_len)
{
    World w;
    SnapshotTree *t, *again;
    VmInstance *v;

    world_create(&w, dir);
    world_mark(&w);
    world_open(&w, &t, &v);
    if (FAILED(snapshot_take(t, v, L"s1")) || FAILED(snapshot_take(t, v, L"s2")) ||
        FAILED(snapshot_new_branch(t, v, 0))) {
        swprintf_s(why, why_len, L"setting up failed");
    } else {
        again = (SnapshotTree *)calloc(1, sizeof(SnapshotTree));
        if (!again) fatal(L"out of memory");
        snapshot_init(again, w.snap_dir);
        if (memcmp(t, again, sizeof(SnapshotTree)) != 0)
            swprintf_s(why, why_len, L"the tree read back differs from the one in memory");
        free(again);
    }
    world_close(&w, t, v, FALSE);
}

/* A damaged or missing tree.dat must not move a VM off a disk that exists: if that disk
   then lands on a parent, every child of the parent stops opening. */
static void test_damaged_tree_keeps_vm(const wchar_t *dir, wchar_t *why, size_t why_len)
{
    World w;
    SnapshotTree *t;
    VmInstance *v;
    wchar_t tree_path[MAX_PATH], before_path[MAX_PATH];
    HelperReply rep;
    unsigned before = 0, after = 0;
    HRESULT hr;
    int round;

    world_create(&w, dir);
    setup_nested(&w);
    swprintf_s(tree_path, MAX_PATH, L"%s\\tree.dat", w.snap_dir);

    for (round = 0; round < 2 && !why[0]; round++) {
        if (round == 0) {
            /* Cut short, as a crash during an in-place save used to leave it */
            FILE *f;
            if (_wfopen_s(&f, tree_path, L"r, ccs=UTF-8") != 0 || !f) fatal(L"cannot read %s", tree_path);
            {
                wchar_t first[1024], second[1024];
                if (!fgetws(first, 1024, f) || !fgetws(second, 1024, f)) fatal(L"tree.dat too short");
                fclose(f);
                if (_wfopen_s(&f, tree_path, L"w, ccs=UTF-8") != 0 || !f) fatal(L"cannot write %s", tree_path);
                fputws(first, f);
                fputws(second, f);
                fputws(L"\n[Snapshot]\nGuid=x\nName=x\nVhdx=", f);
                fputws(w.base, f);
                fputws(L"\n", f);
                fclose(f);
            }
        } else {
            /* Gone altogether */
            DeleteFileW(tree_path);
        }
        world_open(&w, &t, &v);
        wcscpy_s(before_path, MAX_PATH, v->vhdx_path);
        if (visible_mask(before_path, w.markers, &before, &rep) != ERROR_SUCCESS)
            fatal(L"cannot read %s", before_path);
        hr = snapshot_ensure_writable(t, v);
        if (FAILED(hr)) {
            swprintf_s(why, why_len, L"round %d: the start fails 0x%08X", round, hr);
        } else if (!same_path(before_path, v->vhdx_path)) {
            swprintf_s(why, why_len, L"round %d: the VM was moved off its disk to %s", round, leaf(v->vhdx_path));
        } else if (visible_mask(v->vhdx_path, w.markers, &after, &rep) != ERROR_SUCCESS || after != before) {
            swprintf_s(why, why_len, L"round %d: the VM's disk no longer shows the same", round);
        }
        world_close(&w, t, v, FALSE);
    }
}

/* What matters in a power cut, which no process kill can show: a new disk reaches the
   disk before tree.dat names it, and tree.dat before it replaces the old one. This reads
   the recorded operations, so it shows the calls are made in that order, not that they
   are enough. */
static void test_durable_order(const wchar_t *dir, wchar_t *why, size_t why_len)
{
    static Site sites[MAX_SITES];
    wchar_t trace[MAX_PATH];
    int n, i, unflushed_disks = 0;
    BOOL committed = FALSE, replaced = FALSE;

    (void)dir;
    swprintf_s(trace, MAX_PATH, L"%s\\take-on-frozen-snap.trace", g_run_dir);
    if (!exists(trace)) return; /* a filtered run */
    n = load_trace(trace, sites, MAX_SITES);
    for (i = 0; i < n; i++) {
        if (wcscmp(sites[i].name, L"vhdx_create_differencing") == 0) {
            unflushed_disks++;
        } else if (wcscmp(sites[i].name, L"FlushFileBuffers") == 0) {
            unflushed_disks--;
        } else if (wcscmp(sites[i].name, L"_commit") == 0) {
            committed = TRUE;
        } else if (wcscmp(sites[i].name, L"MoveFileExW") == 0) {
            replaced = TRUE;
            if (!committed)
                swprintf_s(why, why_len, L"tree.dat replaces the old one before being flushed");
            else if (unflushed_disks > 0)
                swprintf_s(why, why_len, L"tree.dat is committed naming %d disk(s) not flushed", unflushed_disks);
        }
    }
    if (!replaced)
        swprintf_s(why, why_len, L"tree.dat is rewritten in place, not replaced");
}

static int run_behaviour(void)
{
    static const struct { const wchar_t *name; BehaviourTest fn; } TESTS[] = {
        { L"snapshots-hold-their-state", test_snapshots_hold_their_state },
        { L"old-tree.dat-round-trips",   test_old_tree_dat },
        { L"delete-guard",               test_delete_guard },
        { L"restart-preserves-tree",     test_restart_preserves_tree },
        { L"durable-order",              test_durable_order },
        { L"damaged-tree-keeps-vm",      test_damaged_tree_keeps_vm },
    };
    int i, failed = 0;

    say(L"\nBehaviour with nothing going wrong:\n");
    for (i = 0; i < (int)(sizeof(TESTS) / sizeof(TESTS[0])); i++) {
        wchar_t dir[MAX_PATH], why[260];
        why[0] = L'\0';
        swprintf_s(dir, MAX_PATH, L"%s\\behaviour-%d", g_run_dir, i);
        TESTS[i].fn(dir, why, 260);
        say(L"  %s  %-28s %s\n", why[0] ? L"FAIL" : L"ok  ", TESTS[i].name, why);
        if (why[0]) failed++;
        if (!g_keep) remove_tree(dir);
    }
    return failed;
}

/* ---- Run bookkeeping ---- */

static void write_marker_file(const wchar_t *name, const wchar_t *format, ...)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    va_list args;
    swprintf_s(path, MAX_PATH, L"%s\\%s", g_run_dir, name);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) fatal(L"cannot write %s", path);
    va_start(args, format);
    vfwprintf(f, format, args);
    va_end(args);
    fflush(f);
    _commit(_fileno(f));
    fclose(f);
}

static void summarise(const wchar_t *label, int behaviour_failed)
{
    int i;
    say(L"\n=== %s: %d cases, %d with a failed check ===\n", label, g_cases, g_cases_failed);
    for (i = 0; i < CHECK_COUNT; i++)
        say(L"  %-10s failed in %4d   %s\n", CHECK_NAME[i], g_check_failed[i], CHECK_MEANING[i]);
    say(L"  behaviour  failed     %d\n", behaviour_failed);
    say(L"\nPaused %d time(s) for the Windows taskbar, %llu s in all.\n", g_shell_pauses, g_shell_paused_ms / 1000);
}

static int run_main(int argc, wchar_t **argv)
{
    wchar_t tsv[MAX_PATH], summary[MAX_PATH], source[MAX_PATH], line[512];
    const wchar_t *label;
    int i, behaviour_failed, selftest;
    FILE *f;

    /* run <run folder> <label> [--filter text] [--keep] */
    if (argc < 4) return 2;
    if (GetFullPathNameW(argv[2], MAX_PATH, g_run_dir, NULL) == 0) return 2;
    label = argv[3];
    for (i = 4; i < argc; i++) {
        if (wcscmp(argv[i], L"--keep") == 0) g_keep = TRUE;
        else if (wcscmp(argv[i], L"--filter") == 0 && i + 1 < argc) g_filter = argv[++i];
    }
    CreateDirectoryW(g_run_dir, NULL);
    fault_set_fence(g_run_dir);

    /* Which code, on which boot. A reset between these lines and COMPLETE voids the run. */
    source[0] = L'\0';
    line[0] = L'\0';
    swprintf_s(source, MAX_PATH, L"%s", g_exe);
    {
        wchar_t *slash = wcsrchr(source, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - source), L"source.txt");
    }
    if (_wfopen_s(&f, source, L"r") == 0 && f) {
        if (!fgetws(line, 512, f)) line[0] = L'\0';
        fclose(f);
    }
    write_marker_file(L"STARTED", L"label %s\nboot-age-ms %llu\nsource %s\n", label, GetTickCount64(), line);

    swprintf_s(summary, MAX_PATH, L"%s\\summary.txt", g_run_dir);
    _wfopen_s(&g_summary, summary, L"w");

    say(L"Run: %s\nLabel: %s\nSource: %s\n", g_run_dir, label, line);
    say(L"\nThe disk helper's own checks:\n");
    selftest = disk_helper_selftest(g_run_dir);
    if (selftest != 0) fatal(L"the disk helper's self-test failed (%d)", selftest);
    say(L"All passed.\n");

    swprintf_s(tsv, MAX_PATH, L"%s\\results.tsv", g_run_dir);
    if (_wfopen_s(&g_tsv, tsv, L"w") != 0 || !g_tsv) fatal(L"cannot write %s", tsv);
    fwprintf(g_tsv, L"scenario\tevent\toutcome");
    for (i = 0; i < CHECK_COUNT; i++) fwprintf(g_tsv, L"\t%s", CHECK_NAME[i]);
    fwprintf(g_tsv, L"\twhy\n");

    say(L"\nThe matrix:\n");
    run_matrix();
    behaviour_failed = run_behaviour();
    fclose(g_tsv);
    g_tsv = NULL;

    summarise(label, behaviour_failed);

    if (g_cases != g_planned)
        fatal(L"ran %d cases, planned %d", g_cases, g_planned);
    write_marker_file(L"COMPLETE", L"cases %d\nplanned %d\ncases-failed %d\nbehaviour-failed %d\nboot-age-ms %llu\n",
                      g_cases, g_planned, g_cases_failed, behaviour_failed, GetTickCount64());
    return (g_cases_failed || behaviour_failed) ? 1 : 0;
}

/* ---- Open questions about VHDX itself, measured ---- */

static BOOL hashes_equal(const BYTE *a, const BYTE *b)
{
    return memcmp(a, b, 32) == 0;
}

static int measure_main(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH], base[MAX_PATH], child[MAX_PATH], grandchild[MAX_PATH];
    HelperReply rep;
    BYTE h1[32], h2[32], h3[32];
    char text[HELPER_RECORD_SIZE];
    DWORD r;
    HRESULT hr;

    /* measure <folder> */
    if (argc != 3) return 2;
    if (GetFullPathNameW(argv[2], MAX_PATH, dir, NULL) == 0) return 2;
    CreateDirectoryW(dir, NULL);
    fault_set_fence(dir);
    swprintf_s(base, MAX_PATH, L"%s\\base.vhdx", dir);
    swprintf_s(child, MAX_PATH, L"%s\\child.vhdx", dir);
    swprintf_s(grandchild, MAX_PATH, L"%s\\grandchild.vhdx", dir);

    wprintf(L"Q1. Does a parent stay byte-identical while its child is attached and written?\n");
    make_base(base);
    disk_write(base, MARKER_STEP, "written to base", &rep);
    hr = vhdx_create_differencing_for_tests(child, base);
    wprintf(L"    create child: 0x%08X\n", hr);
    hash_file(base, h1);
    r = disk_hold(child, FALSE, &rep);
    wprintf(L"    child attached read-write: error %lu\n", r);
    if (hash_file(base, h2))
        wprintf(L"    parent while the child is attached: %s\n", hashes_equal(h1, h2) ? L"identical" : L"CHANGED");
    else
        wprintf(L"    parent while the child is attached: could not be read\n");
    disk_release(&rep);
    r = disk_write(child, 2 * MARKER_STEP, "written to child", &rep);
    wprintf(L"    write through child: error %lu\n", r);
    hash_file(base, h3);
    wprintf(L"    parent after the child was written: %s\n", hashes_equal(h1, h3) ? L"identical" : L"CHANGED");

    wprintf(L"\nQ2. What if a frozen parent is written to anyway?\n");
    r = disk_try_attach(base, FALSE, &rep);
    wprintf(L"    attach the parent read-write, write nothing: error %lu\n", r);
    hash_file(base, h2);
    wprintf(L"    parent's bytes after that: %s\n", hashes_equal(h1, h2) ? L"identical" : L"CHANGED");
    r = disk_try_attach(child, TRUE, &rep);
    wprintf(L"    child attaches after that: error %lu%s\n", r, r ? L"  <- refused" : L"");
    r = disk_write(base, 3 * MARKER_STEP, "late write to parent", &rep);
    wprintf(L"    write to the parent: error %lu\n", r);
    hash_file(base, h2);
    wprintf(L"    parent's bytes after that: %s\n", hashes_equal(h1, h2) ? L"identical" : L"CHANGED");
    r = disk_try_attach(child, TRUE, &rep);
    wprintf(L"    child attaches after that: error %lu%s  %s\n", r, r ? L"  <- refused" : L"", r ? rep.text : L"");
    r = disk_read(child, 3 * MARKER_STEP, text, sizeof(text), &rep);
    wprintf(L"    read of the late write through the child: error %lu, \"%S\"\n", r, text);

    wprintf(L"\nQ3. Would a read-only file attribute on frozen disks work, and enforce anything?\n");
    DeleteFileW(child);
    DeleteFileW(base);
    make_base(base);
    disk_write(base, MARKER_STEP, "written to base", &rep);
    SetFileAttributesW(base, FILE_ATTRIBUTE_READONLY);
    hash_file(base, h1);
    hr = vhdx_create_differencing_for_tests(child, base);
    wprintf(L"    create a child of a read-only parent: 0x%08X\n", hr);
    r = disk_write(child, 2 * MARKER_STEP, "written to child", &rep);
    wprintf(L"    attach and write through that child: error %lu  %s\n", r, r ? rep.text : L"");
    r = disk_read(child, MARKER_STEP, text, sizeof(text), &rep);
    wprintf(L"    read the parent's data through the child: error %lu, \"%S\"\n", r, text);
    hash_file(base, h2);
    wprintf(L"    parent's bytes: %s\n", hashes_equal(h1, h2) ? L"identical" : L"CHANGED");
    r = disk_write(base, 3 * MARKER_STEP, "late write to parent", &rep);
    wprintf(L"    write straight to the read-only parent: error %lu  %s\n", r, r ? rep.text : L"(it was allowed)");
    hash_file(base, h2);
    wprintf(L"    parent's bytes after that: %s\n", hashes_equal(h1, h2) ? L"identical" : L"CHANGED");
    wprintf(L"    DeleteFileW on the read-only parent: %s\n", DeleteFileW(base) ? L"deleted" : L"refused");
    SetFileAttributesW(base, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(grandchild);
    DeleteFileW(child);
    DeleteFileW(base);
    RemoveDirectoryW(dir);
    return 0;
}

/* How often does a real operation fail by itself? Nothing is forced here, and no disk is
   attached, so this measures what a scanner or the file system do to these calls. */
static int stress_main(int argc, wchar_t **argv)
{
    wchar_t dir[MAX_PATH];
    World w;
    SnapshotTree *t;
    VmInstance *v;
    int rounds, i, failures = 0;
    ULONGLONG started;

    /* stress <folder> <rounds> */
    if (argc != 4) return 2;
    if (GetFullPathNameW(argv[2], MAX_PATH, dir, NULL) == 0) return 2;
    rounds = _wtoi(argv[3]);
    fault_set_fence(dir);
    world_create(&w, dir);
    world_open(&w, &t, &v);
    if (FAILED(snapshot_take(t, v, L"s1"))) fatal(L"cannot take the first snapshot");

    started = GetTickCount64();
    for (i = 0; i < rounds; i++) {
        HRESULT hr = snapshot_new_branch(t, v, 0);
        if (FAILED(hr)) {
            wprintf(L"round %d: new branch 0x%08X at %S\n", i, hr, fault_last_op());
            failures++;
            continue;
        }
        hr = snapshot_delete_branch(t, v, 0, t->nodes[0].branch_count - 1);
        if (FAILED(hr)) {
            wprintf(L"round %d: delete branch 0x%08X at %S\n", i, hr, fault_last_op());
            failures++;
        }
    }
    wprintf(L"%d rounds, %d saves, %d failures, %llu ms\n", rounds, rounds * 2, failures,
            GetTickCount64() - started);
    world_close(&w, t, v, FALSE);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    if (argc >= 2 && wcscmp(argv[1], L"stress") == 0) return stress_main(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"child") == 0) return child_main(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"run") == 0) return run_main(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"measure") == 0) return measure_main(argc, argv);
    wprintf(L"usage:\n  snapshot_tests run <run folder> <label> [--filter text] [--keep]\n"
            L"  snapshot_tests measure <folder>\n");
    return 2;
}
