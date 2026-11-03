#include "snapshot.h"
#include "disk_util.h"
#include <stdio.h>
#include <io.h>
#include <errno.h>
#include <objbase.h>

/* ---- Helpers ---- */

static void generate_guid_string(wchar_t *out, size_t out_len)
{
    GUID g = {0};
    if (FAILED(CoCreateGuid(&g))) {
        if (out_len > 0) out[0] = L'\0';
        return;
    }
    swprintf_s(out, out_len,
        L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static void make_datetime_name(wchar_t *out, size_t max)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(out, max, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* Get branch/base_branch arrays and count for a given index */
static BOOL get_branch_list(SnapshotTree *tree, int index,
    BranchEntry **out_branches, int **out_count, const wchar_t **out_parent)
{
    if (index == -2) {
        *out_branches = tree->base_branches;
        *out_count = &tree->base_branch_count;
        *out_parent = tree->base_vhdx;
        return TRUE;
    }
    if (index >= 0 && index < tree->count && tree->nodes[index].valid) {
        *out_branches = tree->nodes[index].branches;
        *out_count = &tree->nodes[index].branch_count;
        *out_parent = tree->nodes[index].snap_vhdx;
        return TRUE;
    }
    return FALSE;
}

/* Creates a differencing disk and forces it to the disk: tree.dat is about to name it,
   and must not get there first. */
static HRESULT create_child_disk(const wchar_t *child_path, const wchar_t *parent_path)
{
    HANDLE h;
    HRESULT hr = vhdx_create_differencing(child_path, parent_path);
    if (FAILED(hr)) return hr;

    h = CreateFileW(child_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    } else {
        if (!FlushFileBuffers(h)) hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(h);
    }
    if (FAILED(hr)) DeleteFileW(child_path);
    return hr;
}

/* ---- Persistence (tree.dat) ---- */

/*  Format:
 *    [Base]
 *    C:\...\disk.vhdx
 *    [BaseBranch]
 *    Guid=abc12345-...
 *    Name=2026-03-21 14:30:00
 *    Vhdx=C:\...\snapshots\branch_abc12345-....vhdx
 *    [Snapshot]
 *    Guid=def67890-...
 *    Name=Snapshot 1
 *    Vhdx=C:\...\snapshots\snapshot_def67890-....vhdx
 *    Created=<FILETIME as decimal uint64>
 *    Parent=abc99999-...          (only when taken on another snapshot)
 *    [Branch]
 *    Guid=ghi11111-...
 *    Name=2026-03-21 14:35:00
 *    Vhdx=C:\...\snapshots\branch_ghi11111-....vhdx
 *
 *  A snapshot's Vhdx= can name a branch_ file: a working branch that was frozen keeps
 *  its name. What a file is depends on the section naming it, never on the file's name.
 */

/* Something can hold the old file for a moment, a virus scanner for one: about one save
   in 2000 here is refused, then fine. A file that stays refused fails after half a second. */
static BOOL replace_file(const wchar_t *from, const wchar_t *to)
{
    DWORD error = ERROR_SUCCESS;
    int tries;

    for (tries = 0; tries < 10; tries++) {
        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return TRUE;
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(50);
    }
    SetLastError(error);
    return FALSE;
}

/* Replaces tree.dat whole, as save_vm_list does vms.cfg: at any moment the file is the
   old tree or the new one, so replacing it is what commits a change to the tree. */
HRESULT snapshot_save(SnapshotTree *tree)
{
    wchar_t path[MAX_PATH], temporary[MAX_PATH];
    FILE *f;
    int i, b;
    BOOL ok;
    HRESULT hr;

    if (tree->load_failed) return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);

    swprintf_s(path, MAX_PATH, L"%s\\tree.dat", tree->base_dir);
    if (tree->count == 0 && tree->base_branch_count == 0) {
        if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
            return HRESULT_FROM_WIN32(GetLastError());
        tree->base_vhdx[0] = L'\0';
        return S_OK;
    }
    if (!GetTempFileNameW(tree->base_dir, L"asb", 0, temporary))
        return HRESULT_FROM_WIN32(GetLastError());
    if (_wfopen_s(&f, temporary, L"w, ccs=UTF-8") != 0 || !f) {
        DeleteFileW(temporary);
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }

    fwprintf(f, L"[Base]\n%s\n\n", tree->base_vhdx);

    for (b = 0; b < tree->base_branch_count; b++) {
        if (!tree->base_branches[b].valid) continue;
        fwprintf(f, L"[BaseBranch]\n");
        fwprintf(f, L"Guid=%s\n", tree->base_branches[b].guid);
        fwprintf(f, L"Name=%s\n", tree->base_branches[b].friendly_name);
        fwprintf(f, L"Vhdx=%s\n\n", tree->base_branches[b].vhdx_path);
    }

    for (i = 0; i < tree->count; i++) {
        if (!tree->nodes[i].valid) continue;
        fwprintf(f, L"[Snapshot]\n");
        fwprintf(f, L"Guid=%s\n", tree->nodes[i].guid);
        fwprintf(f, L"Name=%s\n", tree->nodes[i].name);
        fwprintf(f, L"Vhdx=%s\n", tree->nodes[i].snap_vhdx);
        {
            ULARGE_INTEGER ft;
            ft.LowPart  = tree->nodes[i].created.dwLowDateTime;
            ft.HighPart = tree->nodes[i].created.dwHighDateTime;
            fwprintf(f, L"Created=%llu\n", ft.QuadPart);
        }
        if (tree->nodes[i].parent_guid[0] != L'\0')
            fwprintf(f, L"Parent=%s\n", tree->nodes[i].parent_guid);
        fwprintf(f, L"\n");

        for (b = 0; b < tree->nodes[i].branch_count; b++) {
            if (!tree->nodes[i].branches[b].valid) continue;
            fwprintf(f, L"[Branch]\n");
            fwprintf(f, L"Guid=%s\n", tree->nodes[i].branches[b].guid);
            fwprintf(f, L"Name=%s\n", tree->nodes[i].branches[b].friendly_name);
            fwprintf(f, L"Vhdx=%s\n\n", tree->nodes[i].branches[b].vhdx_path);
        }
    }

    ok = !ferror(f) && fflush(f) == 0 && _commit(_fileno(f)) == 0;
    if (fclose(f) != 0) ok = FALSE;
    if (!ok) {
        DeleteFileW(temporary);
        return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    if (!replace_file(temporary, path)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        DeleteFileW(temporary);
        return hr;
    }
    return S_OK;
}

static void snapshot_load(SnapshotTree *tree)
{
    wchar_t path[MAX_PATH];
    wchar_t line[1024];
    FILE *f;
    SnapNode *node = NULL;
    BranchEntry *cur_branch = NULL;
    int section = 0; /* 0=none, 1=base, 2=basebranch, 3=snapshot, 4=branch */
    errno_t open_error;

    swprintf_s(path, MAX_PATH, L"%s\\tree.dat", tree->base_dir);
    open_error = _wfopen_s(&f, path, L"r, ccs=UTF-8");
    if (open_error != 0 || !f) {
        if (open_error != ENOENT) tree->load_failed = TRUE;
        return;
    }

    while (fgetws(line, 1024, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r'))
            line[--len] = L'\0';
        if (len == 0) continue;

        if (wcscmp(line, L"[Base]") == 0) {
            section = 1; node = NULL; cur_branch = NULL; continue;
        }
        if (wcscmp(line, L"[BaseBranch]") == 0) {
            section = 2;
            cur_branch = NULL;
            if (tree->base_branch_count < MAX_BRANCHES) {
                cur_branch = &tree->base_branches[tree->base_branch_count];
                ZeroMemory(cur_branch, sizeof(BranchEntry));
                cur_branch->valid = TRUE;
                tree->base_branch_count++;
            }
            continue;
        }
        if (wcscmp(line, L"[Snapshot]") == 0) {
            section = 3;
            cur_branch = NULL;
            if (tree->count < MAX_SNAPSHOTS) {
                node = &tree->nodes[tree->count];
                ZeroMemory(node, sizeof(SnapNode));
                node->valid = TRUE;
                tree->count++;
            } else {
                node = NULL;
            }
            continue;
        }
        if (wcscmp(line, L"[Branch]") == 0) {
            section = 4;
            cur_branch = NULL;
            if (node && node->branch_count < MAX_BRANCHES) {
                cur_branch = &node->branches[node->branch_count];
                ZeroMemory(cur_branch, sizeof(BranchEntry));
                cur_branch->valid = TRUE;
                node->branch_count++;
            }
            continue;
        }

        if (section == 1 && tree->base_vhdx[0] == L'\0') {
            wcscpy_s(tree->base_vhdx, MAX_PATH, line);
            section = 0;
            continue;
        }

        if (section == 2 && cur_branch) {
            if (wcsncmp(line, L"Guid=", 5) == 0)
                wcscpy_s(cur_branch->guid, 64, line + 5);
            else if (wcsncmp(line, L"Name=", 5) == 0)
                wcscpy_s(cur_branch->friendly_name, 128, line + 5);
            else if (wcsncmp(line, L"Vhdx=", 5) == 0)
                wcscpy_s(cur_branch->vhdx_path, MAX_PATH, line + 5);
            continue;
        }

        if (section == 4 && cur_branch) {
            if (wcsncmp(line, L"Guid=", 5) == 0)
                wcscpy_s(cur_branch->guid, 64, line + 5);
            else if (wcsncmp(line, L"Name=", 5) == 0)
                wcscpy_s(cur_branch->friendly_name, 128, line + 5);
            else if (wcsncmp(line, L"Vhdx=", 5) == 0)
                wcscpy_s(cur_branch->vhdx_path, MAX_PATH, line + 5);
            continue;
        }

        if (section != 3 || !node) continue;

        if (wcsncmp(line, L"Guid=", 5) == 0)
            wcscpy_s(node->guid, 64, line + 5);
        else if (wcsncmp(line, L"Name=", 5) == 0)
            wcscpy_s(node->name, 128, line + 5);
        else if (wcsncmp(line, L"Vhdx=", 5) == 0)
            wcscpy_s(node->snap_vhdx, MAX_PATH, line + 5);
        else if (wcsncmp(line, L"Created=", 8) == 0) {
            ULARGE_INTEGER ft;
            ft.QuadPart = _wcstoui64(line + 8, NULL, 10);
            node->created.dwLowDateTime  = ft.LowPart;
            node->created.dwHighDateTime = ft.HighPart;
        }
        else if (wcsncmp(line, L"Parent=", 7) == 0)
            wcscpy_s(node->parent_guid, 64, line + 7);
    }

    fclose(f);
}

/* A tree.dat that exists but cannot be read is not an empty tree: taken for one, a frozen
   disk would be booted, and the file overwritten. It may only be locked for now, so
   every operation that needs the tree tries again. */
static HRESULT tree_ready(SnapshotTree *tree)
{
    if (tree->load_failed) {
        tree->load_failed = FALSE;
        snapshot_load(tree);
    }
    return tree->load_failed ? HRESULT_FROM_WIN32(ERROR_OPEN_FAILED) : S_OK;
}

/* Points a VM whose disk is going away at another branch, or else the base.
   skip: a snapshot whose branches are going too, or -1. */
static void fall_back(SnapshotTree *tree, VmInstance *instance, int skip)
{
    int i, b;

    /* Try base branches first, then other snapshot branches */
    for (b = 0; b < tree->base_branch_count; b++) {
        if (tree->base_branches[b].valid) {
            wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_branches[b].vhdx_path);
            return;
        }
    }
    for (i = 0; i < tree->count; i++) {
        if (i == skip || !tree->nodes[i].valid) continue;
        for (b = 0; b < tree->nodes[i].branch_count; b++) {
            if (tree->nodes[i].branches[b].valid) {
                wcscpy_s(instance->vhdx_path, MAX_PATH, tree->nodes[i].branches[b].vhdx_path);
                return;
            }
        }
    }
    wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_vhdx);
}

/* ---- Public API ---- */

void snapshot_init(SnapshotTree *tree, const wchar_t *base_dir)
{
    ZeroMemory(tree, sizeof(SnapshotTree));
    wcscpy_s(tree->base_dir, MAX_PATH, base_dir);
    CreateDirectoryW(base_dir, NULL);
    snapshot_load(tree);
    if (tree->count == 0 && tree->base_branch_count == 0 && tree->base_vhdx[0] != L'\0')
        snapshot_save(tree);
}

/* No existing disk is moved or written here, and the tree and the VM change only once
   tree.dat is replaced. Until then the worst a crash leaves is an unused new disk. After
   it, vms.cfg can still name the frozen disk, and snapshot_ensure_writable gives such a
   VM a branch at its next start. */
HRESULT snapshot_take(SnapshotTree *tree, VmInstance *instance, const wchar_t *name)
{
    SnapNode *node;
    wchar_t snap_guid[64];
    wchar_t branch_guid[64];
    wchar_t vhdx_path[MAX_PATH];
    wchar_t branch_path[MAX_PATH];
    wchar_t parent_guid[64] = L"";
    wchar_t saved_base[MAX_PATH];
    BranchEntry saved_branches[MAX_BRANCHES];
    BranchEntry *cur_branches = NULL;
    int *cur_count = NULL;
    const wchar_t *cur_parent;
    const wchar_t *base;
    int cur_snap, cur_branch, saved_count = 0, i;
    HRESULT hr;

    if (!tree || !instance || !name)
        return E_INVALIDARG;
    if (tree->count >= MAX_SNAPSHOTS)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    if (instance->running)
        return E_NOT_VALID_STATE;
    hr = tree_ready(tree);
    if (FAILED(hr)) return hr;

    /* The first snapshot captures the base VHDX */
    base = tree->base_vhdx[0] != L'\0' ? tree->base_vhdx : instance->vhdx_path;

    generate_guid_string(snap_guid, 64);

    /* The snapshot must hold what the VM runs on now, not the base: a fork of
       the base would silently leave everything since the first snapshot on a
       branch the VM no longer boots. */
    snapshot_find_current(tree, instance->vhdx_path, &cur_snap, &cur_branch);
    if (cur_branch >= 0) {
        /* On a working branch: that branch's disk becomes the snapshot's, under the
           name it has. It leaves the branch list, because booting it again would
           write to a disk the new branch depends on. */
        if (!get_branch_list(tree, cur_snap, &cur_branches, &cur_count, &cur_parent))
            return E_UNEXPECTED;
        wcscpy_s(vhdx_path, MAX_PATH, instance->vhdx_path);
        if (cur_snap >= 0)
            wcscpy_s(parent_guid, 64, tree->nodes[cur_snap].guid);
    } else {
        /* On a frozen disk (the base, or a snapshot's own disk): it already is
           the current state, so the snapshot forks it. Anything else is a disk
           this tree does not know, and forking the base would lose it. */
        if (_wcsicmp(instance->vhdx_path, base) != 0) {
            for (i = 0; i < tree->count; i++) {
                if (tree->nodes[i].valid &&
                    _wcsicmp(instance->vhdx_path, tree->nodes[i].snap_vhdx) == 0)
                    break;
            }
            if (i == tree->count)
                return E_NOT_VALID_STATE;
            wcscpy_s(parent_guid, 64, tree->nodes[i].guid);
        }
        swprintf_s(vhdx_path, MAX_PATH, L"%s\\snapshot_%s.vhdx", tree->base_dir, snap_guid);
        hr = create_child_disk(vhdx_path, instance->vhdx_path);
        if (FAILED(hr)) return hr;
    }

    /* Auto-create first branch with GUID filename */
    generate_guid_string(branch_guid, 64);
    swprintf_s(branch_path, MAX_PATH, L"%s\\branch_%s.vhdx", tree->base_dir, branch_guid);

    hr = create_child_disk(branch_path, vhdx_path);
    if (FAILED(hr)) {
        if (!cur_branches) DeleteFileW(vhdx_path);
        return hr;
    }

    CopyMemory(saved_base, tree->base_vhdx, sizeof(saved_base));
    if (tree->base_vhdx[0] == L'\0')
        wcscpy_s(tree->base_vhdx, MAX_PATH, instance->vhdx_path);

    /* The frozen branch is the snapshot now */
    if (cur_branches) {
        CopyMemory(saved_branches, cur_branches, sizeof(saved_branches));
        saved_count = *cur_count;
        for (i = cur_branch; i < *cur_count - 1; i++)
            cur_branches[i] = cur_branches[i + 1];
        ZeroMemory(&cur_branches[*cur_count - 1], sizeof(BranchEntry));
        (*cur_count)--;
    }

    /* Record snapshot */
    node = &tree->nodes[tree->count];
    ZeroMemory(node, sizeof(SnapNode));
    wcscpy_s(node->guid, 64, snap_guid);
    wcscpy_s(node->name, 128, name);
    wcscpy_s(node->snap_vhdx, MAX_PATH, vhdx_path);
    wcscpy_s(node->parent_guid, 64, parent_guid);
    GetSystemTimeAsFileTime(&node->created);
    node->valid = TRUE;

    /* Record first branch */
    wcscpy_s(node->branches[0].guid, 64, branch_guid);
    wcscpy_s(node->branches[0].friendly_name, 128, L"Default Branch");
    wcscpy_s(node->branches[0].vhdx_path, MAX_PATH, branch_path);
    node->branches[0].valid = TRUE;
    node->branch_count = 1;

    tree->count++;

    hr = snapshot_save(tree);
    if (FAILED(hr)) {
        tree->count--;
        ZeroMemory(node, sizeof(SnapNode));
        if (cur_branches) {
            CopyMemory(cur_branches, saved_branches, sizeof(saved_branches));
            *cur_count = saved_count;
        }
        CopyMemory(tree->base_vhdx, saved_base, sizeof(saved_base));
        DeleteFileW(branch_path);
        if (!cur_branches) DeleteFileW(vhdx_path);
        return hr;
    }

    /* Switch VM to the branch */
    wcscpy_s(instance->vhdx_path, MAX_PATH, branch_path);
    return S_OK;
}

HRESULT snapshot_new_branch(SnapshotTree *tree, VmInstance *instance, int index)
{
    BranchEntry *branches;
    int *branch_count;
    const wchar_t *parent_vhdx;
    wchar_t guid[64];
    wchar_t branch_path[MAX_PATH];
    HRESULT hr;

    if (!tree || !instance) return E_INVALIDARG;
    hr = tree_ready(tree);
    if (FAILED(hr)) return hr;
    if (tree->base_vhdx[0] == L'\0') return E_NOT_VALID_STATE;
    if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
        return E_INVALIDARG;
    if (*branch_count >= MAX_BRANCHES)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);

    generate_guid_string(guid, 64);
    swprintf_s(branch_path, MAX_PATH, L"%s\\branch_%s.vhdx", tree->base_dir, guid);

    hr = create_child_disk(branch_path, parent_vhdx);
    if (FAILED(hr)) return hr;

    wcscpy_s(branches[*branch_count].guid, 64, guid);
    make_datetime_name(branches[*branch_count].friendly_name, 128);
    wcscpy_s(branches[*branch_count].vhdx_path, MAX_PATH, branch_path);
    branches[*branch_count].valid = TRUE;
    (*branch_count)++;

    hr = snapshot_save(tree);
    if (FAILED(hr)) {
        (*branch_count)--;
        ZeroMemory(&branches[*branch_count], sizeof(BranchEntry));
        DeleteFileW(branch_path);
        return hr;
    }

    wcscpy_s(instance->vhdx_path, MAX_PATH, branch_path);
    return S_OK;
}

HRESULT snapshot_select_branch(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx)
{
    BranchEntry *branches;
    int *branch_count;
    const wchar_t *parent_vhdx;
    HRESULT hr;

    if (!tree || !instance) return E_INVALIDARG;
    hr = tree_ready(tree);
    if (FAILED(hr)) return hr;
    if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
        return E_INVALIDARG;
    if (branch_idx < 0 || branch_idx >= *branch_count || !branches[branch_idx].valid)
        return E_INVALIDARG;

    wcscpy_s(instance->vhdx_path, MAX_PATH, branches[branch_idx].vhdx_path);
    return S_OK;
}

HRESULT snapshot_ensure_writable(SnapshotTree *tree, VmInstance *instance)
{
    int i;
    HRESULT hr;

    if (!tree || !instance) return E_INVALIDARG;
    if (instance->running) return E_NOT_VALID_STATE;
    hr = tree_ready(tree);
    if (FAILED(hr)) return hr;

    if ((tree->count > 0 || tree->base_branch_count > 0) &&
        tree->base_vhdx[0] != L'\0' &&
        _wcsicmp(instance->vhdx_path, tree->base_vhdx) == 0)
        return snapshot_new_branch(tree, instance, -2);

    for (i = 0; i < tree->count; i++) {
        if (tree->nodes[i].valid &&
            _wcsicmp(instance->vhdx_path, tree->nodes[i].snap_vhdx) == 0)
            return snapshot_new_branch(tree, instance, i);
    }

    return S_FALSE;
}

HRESULT snapshot_leave_disks(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx)
{
    BranchEntry *branches;
    int *branch_count;
    const wchar_t *parent_vhdx;
    int i, b;
    BOOL on_it;
    HRESULT hr;

    if (!tree || !instance) return E_INVALIDARG;
    hr = tree_ready(tree);
    if (FAILED(hr)) return hr;

    if (branch_idx >= 0) {
        if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
            return E_INVALIDARG;
        if (branch_idx >= *branch_count || !branches[branch_idx].valid)
            return E_INVALIDARG;
        if (_wcsicmp(instance->vhdx_path, branches[branch_idx].vhdx_path) != 0)
            return S_FALSE;

        /* Fall back to a sibling or base */
        for (i = 0; i < *branch_count; i++) {
            if (i != branch_idx && branches[i].valid) {
                wcscpy_s(instance->vhdx_path, MAX_PATH, branches[i].vhdx_path);
                return S_OK;
            }
        }
        wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_vhdx);
        return S_OK;
    }

    if (index < 0 || index >= tree->count) return E_INVALIDARG;
    if (!tree->nodes[index].valid) return E_NOT_VALID_STATE;

    /* A snapshot taken on this one is a differencing child of its disk */
    if (tree->nodes[index].guid[0] != L'\0') {
        for (i = 0; i < tree->count; i++) {
            if (tree->nodes[i].valid &&
                _wcsicmp(tree->nodes[i].parent_guid, tree->nodes[index].guid) == 0)
                return HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY);
        }
    }

    /* Check if currently on any of this snapshot's branches */
    on_it = _wcsicmp(instance->vhdx_path, tree->nodes[index].snap_vhdx) == 0;
    for (b = 0; b < tree->nodes[index].branch_count; b++) {
        if (tree->nodes[index].branches[b].valid &&
            _wcsicmp(instance->vhdx_path, tree->nodes[index].branches[b].vhdx_path) == 0)
            on_it = TRUE;
    }
    if (!on_it) return S_FALSE;

    fall_back(tree, instance, index);
    return S_OK;
}

HRESULT snapshot_delete(SnapshotTree *tree, VmInstance *instance, int index)
{
    SnapNode saved;
    wchar_t saved_path[MAX_PATH];
    int i, b;
    HRESULT hr;

    if (!tree || !instance) return E_INVALIDARG;
    wcscpy_s(saved_path, MAX_PATH, instance->vhdx_path);
    hr = snapshot_leave_disks(tree, instance, index, -1);
    if (FAILED(hr)) return hr;
    saved = tree->nodes[index];

    /* Shift remaining nodes down */
    for (i = index; i < tree->count - 1; i++)
        tree->nodes[i] = tree->nodes[i + 1];
    ZeroMemory(&tree->nodes[tree->count - 1], sizeof(SnapNode));
    tree->count--;

    /* Forget the disks before deleting them: a crash in between leaves unused files,
       not a tree.dat naming disks that are gone. */
    hr = snapshot_save(tree);
    if (FAILED(hr)) {
        for (i = tree->count; i > index; i--)
            tree->nodes[i] = tree->nodes[i - 1];
        tree->nodes[index] = saved;
        tree->count++;
        wcscpy_s(instance->vhdx_path, MAX_PATH, saved_path);
        return hr;
    }

    /* Delete all branch VHDXs */
    for (b = 0; b < saved.branch_count; b++) {
        if (saved.branches[b].valid)
            DeleteFileW(saved.branches[b].vhdx_path);
    }
    /* Delete the frozen snapshot VHDX */
    DeleteFileW(saved.snap_vhdx);
    return S_OK;
}

HRESULT snapshot_delete_branch(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx)
{
    BranchEntry *branches;
    BranchEntry saved[MAX_BRANCHES];
    int *branch_count;
    const wchar_t *parent_vhdx;
    wchar_t saved_path[MAX_PATH];
    int saved_count;
    int i;
    HRESULT hr;

    if (!tree || !instance || branch_idx < 0) return E_INVALIDARG;
    wcscpy_s(saved_path, MAX_PATH, instance->vhdx_path);
    hr = snapshot_leave_disks(tree, instance, index, branch_idx);
    if (FAILED(hr)) return hr;
    if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
        return E_INVALIDARG;
    CopyMemory(saved, branches, sizeof(saved));
    saved_count = *branch_count;

    /* Shift remaining branches down */
    for (i = branch_idx; i < *branch_count - 1; i++)
        branches[i] = branches[i + 1];
    ZeroMemory(&branches[*branch_count - 1], sizeof(BranchEntry));
    (*branch_count)--;

    /* As in snapshot_delete: forget the disk before deleting it */
    hr = snapshot_save(tree);
    if (FAILED(hr)) {
        CopyMemory(branches, saved, sizeof(saved));
        *branch_count = saved_count;
        wcscpy_s(instance->vhdx_path, MAX_PATH, saved_path);
        return hr;
    }

    DeleteFileW(saved[branch_idx].vhdx_path);
    return S_OK;
}

void snapshot_find_current(SnapshotTree *tree, const wchar_t *vhdx_path, int *snap_idx, int *branch_idx)
{
    int i, b;

    *snap_idx = -1;
    *branch_idx = -1;

    if (!tree || !vhdx_path || vhdx_path[0] == L'\0')
        return;

    /* Check base branches */
    for (b = 0; b < tree->base_branch_count; b++) {
        if (tree->base_branches[b].valid &&
            _wcsicmp(vhdx_path, tree->base_branches[b].vhdx_path) == 0) {
            *snap_idx = -2;
            *branch_idx = b;
            return;
        }
    }

    /* Check snapshot branches */
    for (i = 0; i < tree->count; i++) {
        if (!tree->nodes[i].valid) continue;
        for (b = 0; b < tree->nodes[i].branch_count; b++) {
            if (tree->nodes[i].branches[b].valid &&
                _wcsicmp(vhdx_path, tree->nodes[i].branches[b].vhdx_path) == 0) {
                *snap_idx = i;
                *branch_idx = b;
                return;
            }
        }
    }
}

int snapshot_parent_index(SnapshotTree *tree, int snap_idx)
{
    int i;

    if (!tree || snap_idx < 0 || snap_idx >= tree->count || !tree->nodes[snap_idx].valid)
        return -1;
    if (tree->nodes[snap_idx].parent_guid[0] == L'\0')
        return -2;
    for (i = 0; i < tree->count; i++) {
        if (i != snap_idx && tree->nodes[i].valid &&
            _wcsicmp(tree->nodes[i].guid, tree->nodes[snap_idx].parent_guid) == 0)
            return i;
    }
    return -1;
}

BOOL snapshot_get_branch_time(SnapshotTree *tree, int snap_idx, int branch_idx, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    const wchar_t *path;

    if (!tree || !ft) return FALSE;

    if (snap_idx == -2) {
        if (branch_idx < 0 || branch_idx >= tree->base_branch_count ||
            !tree->base_branches[branch_idx].valid)
            return FALSE;
        path = tree->base_branches[branch_idx].vhdx_path;
    } else if (snap_idx >= 0 && snap_idx < tree->count && tree->nodes[snap_idx].valid) {
        if (branch_idx < 0 || branch_idx >= tree->nodes[snap_idx].branch_count ||
            !tree->nodes[snap_idx].branches[branch_idx].valid)
            return FALSE;
        path = tree->nodes[snap_idx].branches[branch_idx].vhdx_path;
    } else {
        return FALSE;
    }

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return FALSE;
    *ft = fad.ftLastWriteTime;
    return TRUE;
}

HRESULT snapshot_rename(SnapshotTree *tree, int snap_idx, int branch_idx, const wchar_t *new_name)
{
    wchar_t *target;
    wchar_t saved[128];
    HRESULT hr;

    if (!tree || !new_name || new_name[0] == L'\0')
        return E_INVALIDARG;
    hr = tree_ready(tree);
    if (FAILED(hr)) return hr;

    if (snap_idx == -2) {
        /* Rename a base branch */
        if (branch_idx < 0 || branch_idx >= tree->base_branch_count ||
            !tree->base_branches[branch_idx].valid)
            return E_INVALIDARG;
        target = tree->base_branches[branch_idx].friendly_name;
    } else if (snap_idx >= 0 && snap_idx < tree->count && tree->nodes[snap_idx].valid) {
        if (branch_idx < 0) {
            /* Rename the snapshot itself */
            target = tree->nodes[snap_idx].name;
        } else {
            /* Rename a snapshot branch */
            if (branch_idx >= tree->nodes[snap_idx].branch_count ||
                !tree->nodes[snap_idx].branches[branch_idx].valid)
                return E_INVALIDARG;
            target = tree->nodes[snap_idx].branches[branch_idx].friendly_name;
        }
    } else {
        return E_INVALIDARG;
    }

    wcscpy_s(saved, 128, target);
    wcscpy_s(target, 128, new_name);
    hr = snapshot_save(tree);
    if (FAILED(hr)) wcscpy_s(target, 128, saved);
    return hr;
}
