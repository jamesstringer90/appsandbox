#include "snapshot.h"
#include "disk_util.h"
#include <stdio.h>
#include <objbase.h>

/* ---- Helpers ---- */

static void generate_guid_string(wchar_t *out, size_t out_len)
{
    GUID g;
    CoCreateGuid(&g);
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
 *    [Branch]
 *    Guid=ghi11111-...
 *    Name=2026-03-21 14:35:00
 *    Vhdx=C:\...\snapshots\branch_ghi11111-....vhdx
 */

void snapshot_save(SnapshotTree *tree)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    int i, b;

    swprintf_s(path, MAX_PATH, L"%s\\tree.dat", tree->base_dir);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;

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
        fwprintf(f, L"\n");

        for (b = 0; b < tree->nodes[i].branch_count; b++) {
            if (!tree->nodes[i].branches[b].valid) continue;
            fwprintf(f, L"[Branch]\n");
            fwprintf(f, L"Guid=%s\n", tree->nodes[i].branches[b].guid);
            fwprintf(f, L"Name=%s\n", tree->nodes[i].branches[b].friendly_name);
            fwprintf(f, L"Vhdx=%s\n\n", tree->nodes[i].branches[b].vhdx_path);
        }
    }

    fclose(f);
}

static void snapshot_load(SnapshotTree *tree)
{
    wchar_t path[MAX_PATH];
    wchar_t line[1024];
    FILE *f;
    SnapNode *node = NULL;
    BranchEntry *cur_branch = NULL;
    int section = 0; /* 0=none, 1=base, 2=basebranch, 3=snapshot, 4=branch */

    swprintf_s(path, MAX_PATH, L"%s\\tree.dat", tree->base_dir);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;

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
    }

    fclose(f);
}

/* ---- Public API ---- */

void snapshot_init(SnapshotTree *tree, const wchar_t *base_dir)
{
    ZeroMemory(tree, sizeof(SnapshotTree));
    wcscpy_s(tree->base_dir, MAX_PATH, base_dir);
    CreateDirectoryW(base_dir, NULL);
    snapshot_load(tree);
}

HRESULT snapshot_take(SnapshotTree *tree, VmInstance *instance, const wchar_t *name)
{
    SnapNode *node;
    wchar_t snap_guid[64];
    wchar_t branch_guid[64];
    wchar_t vhdx_path[MAX_PATH];
    wchar_t branch_path[MAX_PATH];
    HRESULT hr;

    if (!tree || !instance || !name)
        return E_INVALIDARG;
    if (tree->count >= MAX_SNAPSHOTS)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    if (instance->running)
        return E_NOT_VALID_STATE;

    /* Capture the base VHDX on first snapshot */
    if (tree->base_vhdx[0] == L'\0') {
        wcscpy_s(tree->base_vhdx, MAX_PATH, instance->vhdx_path);
    }

    /* Build frozen snapshot VHDX with GUID filename */
    generate_guid_string(snap_guid, 64);
    swprintf_s(vhdx_path, MAX_PATH, L"%s\\snapshot_%s.vhdx", tree->base_dir, snap_guid);

    hr = vhdx_create_differencing(vhdx_path, tree->base_vhdx);
    if (FAILED(hr)) return hr;

    /* Auto-create first branch with GUID filename */
    generate_guid_string(branch_guid, 64);
    swprintf_s(branch_path, MAX_PATH, L"%s\\branch_%s.vhdx", tree->base_dir, branch_guid);

    hr = vhdx_create_differencing(branch_path, vhdx_path);
    if (FAILED(hr)) {
        DeleteFileW(vhdx_path);
        return hr;
    }

    /* Record snapshot */
    node = &tree->nodes[tree->count];
    ZeroMemory(node, sizeof(SnapNode));
    wcscpy_s(node->guid, 64, snap_guid);
    wcscpy_s(node->name, 128, name);
    wcscpy_s(node->snap_vhdx, MAX_PATH, vhdx_path);
    GetSystemTimeAsFileTime(&node->created);
    node->valid = TRUE;

    /* Record first branch */
    wcscpy_s(node->branches[0].guid, 64, branch_guid);
    wcscpy_s(node->branches[0].friendly_name, 128, L"Default Branch");
    wcscpy_s(node->branches[0].vhdx_path, MAX_PATH, branch_path);
    node->branches[0].valid = TRUE;
    node->branch_count = 1;

    tree->count++;

    /* Switch VM to the branch */
    wcscpy_s(instance->vhdx_path, MAX_PATH, branch_path);

    snapshot_save(tree);
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
    if (tree->base_vhdx[0] == L'\0') return E_NOT_VALID_STATE;
    if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
        return E_INVALIDARG;
    if (*branch_count >= MAX_BRANCHES)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);

    generate_guid_string(guid, 64);
    swprintf_s(branch_path, MAX_PATH, L"%s\\branch_%s.vhdx", tree->base_dir, guid);

    hr = vhdx_create_differencing(branch_path, parent_vhdx);
    if (FAILED(hr)) return hr;

    wcscpy_s(branches[*branch_count].guid, 64, guid);
    make_datetime_name(branches[*branch_count].friendly_name, 128);
    wcscpy_s(branches[*branch_count].vhdx_path, MAX_PATH, branch_path);
    branches[*branch_count].valid = TRUE;
    (*branch_count)++;

    wcscpy_s(instance->vhdx_path, MAX_PATH, branch_path);
    snapshot_save(tree);
    return S_OK;
}

HRESULT snapshot_select_branch(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx)
{
    BranchEntry *branches;
    int *branch_count;
    const wchar_t *parent_vhdx;

    if (!tree || !instance) return E_INVALIDARG;
    if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
        return E_INVALIDARG;
    if (branch_idx < 0 || branch_idx >= *branch_count || !branches[branch_idx].valid)
        return E_INVALIDARG;

    wcscpy_s(instance->vhdx_path, MAX_PATH, branches[branch_idx].vhdx_path);
    return S_OK;
}

HRESULT snapshot_delete(SnapshotTree *tree, VmInstance *instance, int index)
{
    int i, b;
    BOOL need_fallback = FALSE;

    if (!tree || !instance) return E_INVALIDARG;
    if (index < 0 || index >= tree->count) return E_INVALIDARG;
    if (!tree->nodes[index].valid) return E_NOT_VALID_STATE;

    /* Check if currently on any of this snapshot's branches */
    for (b = 0; b < tree->nodes[index].branch_count; b++) {
        if (tree->nodes[index].branches[b].valid &&
            _wcsicmp(instance->vhdx_path, tree->nodes[index].branches[b].vhdx_path) == 0) {
            need_fallback = TRUE;
            break;
        }
    }
    if (_wcsicmp(instance->vhdx_path, tree->nodes[index].snap_vhdx) == 0)
        need_fallback = TRUE;

    if (need_fallback) {
        /* Try base branches first, then other snapshot branches */
        BOOL found = FALSE;
        for (b = 0; b < tree->base_branch_count && !found; b++) {
            if (tree->base_branches[b].valid) {
                wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_branches[b].vhdx_path);
                found = TRUE;
            }
        }
        for (i = 0; i < tree->count && !found; i++) {
            if (i == index || !tree->nodes[i].valid) continue;
            for (b = 0; b < tree->nodes[i].branch_count; b++) {
                if (tree->nodes[i].branches[b].valid) {
                    wcscpy_s(instance->vhdx_path, MAX_PATH, tree->nodes[i].branches[b].vhdx_path);
                    found = TRUE;
                    break;
                }
            }
        }
        if (!found)
            wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_vhdx);
    }

    /* Delete all branch VHDXs */
    for (b = 0; b < tree->nodes[index].branch_count; b++) {
        if (tree->nodes[index].branches[b].valid)
            DeleteFileW(tree->nodes[index].branches[b].vhdx_path);
    }
    /* Delete the frozen snapshot VHDX */
    DeleteFileW(tree->nodes[index].snap_vhdx);

    /* Shift remaining nodes down */
    for (i = index; i < tree->count - 1; i++)
        tree->nodes[i] = tree->nodes[i + 1];
    ZeroMemory(&tree->nodes[tree->count - 1], sizeof(SnapNode));
    tree->count--;

    snapshot_save(tree);
    return S_OK;
}

HRESULT snapshot_delete_branch(SnapshotTree *tree, VmInstance *instance, int index, int branch_idx)
{
    BranchEntry *branches;
    int *branch_count;
    const wchar_t *parent_vhdx;
    int i;

    if (!tree || !instance) return E_INVALIDARG;
    if (!get_branch_list(tree, index, &branches, &branch_count, &parent_vhdx))
        return E_INVALIDARG;
    if (branch_idx < 0 || branch_idx >= *branch_count || !branches[branch_idx].valid)
        return E_INVALIDARG;

    /* If currently on this branch, fall back to a sibling or base */
    if (_wcsicmp(instance->vhdx_path, branches[branch_idx].vhdx_path) == 0) {
        BOOL found = FALSE;
        for (i = 0; i < *branch_count; i++) {
            if (i != branch_idx && branches[i].valid) {
                wcscpy_s(instance->vhdx_path, MAX_PATH, branches[i].vhdx_path);
                found = TRUE;
                break;
            }
        }
        if (!found)
            wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_vhdx);
    }

    DeleteFileW(branches[branch_idx].vhdx_path);

    /* Shift remaining branches down */
    for (i = branch_idx; i < *branch_count - 1; i++)
        branches[i] = branches[i + 1];
    ZeroMemory(&branches[*branch_count - 1], sizeof(BranchEntry));
    (*branch_count)--;

    snapshot_save(tree);
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
    if (!tree || !new_name || new_name[0] == L'\0')
        return E_INVALIDARG;

    if (snap_idx == -2) {
        /* Rename a base branch */
        if (branch_idx < 0 || branch_idx >= tree->base_branch_count ||
            !tree->base_branches[branch_idx].valid)
            return E_INVALIDARG;
        wcscpy_s(tree->base_branches[branch_idx].friendly_name, 128, new_name);
    } else if (snap_idx >= 0 && snap_idx < tree->count && tree->nodes[snap_idx].valid) {
        if (branch_idx < 0) {
            /* Rename the snapshot itself */
            wcscpy_s(tree->nodes[snap_idx].name, 128, new_name);
        } else {
            /* Rename a snapshot branch */
            if (branch_idx >= tree->nodes[snap_idx].branch_count ||
                !tree->nodes[snap_idx].branches[branch_idx].valid)
                return E_INVALIDARG;
            wcscpy_s(tree->nodes[snap_idx].branches[branch_idx].friendly_name, 128, new_name);
        }
    } else {
        return E_INVALIDARG;
    }

    snapshot_save(tree);
    return S_OK;
}
