#include "snapshot.h"
#include "disk_util.h"
#include <stdio.h>

void snapshot_init(SnapshotList *list, const wchar_t *base_dir)
{
    ZeroMemory(list, sizeof(SnapshotList));
    wcscpy_s(list->base_dir, MAX_PATH, base_dir);

    /* Ensure snapshot directory exists */
    CreateDirectoryW(base_dir, NULL);
}

HRESULT snapshot_take(SnapshotList *list, VmInstance *instance, const wchar_t *snapshot_name)
{
    Snapshot *snap;
    wchar_t diff_path[MAX_PATH];
    wchar_t state_path[MAX_PATH];
    HRESULT hr;

    if (!list || !instance || !snapshot_name)
        return E_INVALIDARG;

    if (list->count >= MAX_SNAPSHOTS)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);

    if (!instance->running)
        return E_NOT_VALID_STATE;

    snap = &list->snapshots[list->count];

    /* Build file paths for this snapshot */
    swprintf_s(state_path, MAX_PATH, L"%s\\snap_%d.vmrs", list->base_dir, list->count);
    swprintf_s(diff_path, MAX_PATH, L"%s\\snap_%d.vhdx", list->base_dir, list->count);

    /* 1. Pause the VM */
    hr = hcs_pause_vm(instance);
    if (FAILED(hr)) return hr;

    /* 2. Save VM memory state */
    hr = hcs_save_vm(instance, state_path);
    if (FAILED(hr)) {
        hcs_resume_vm(instance);
        return hr;
    }

    /* 3. Create differencing VHDX
       The current VHDX becomes the parent, new writes go to the diff */
    hr = vhdx_create_differencing(diff_path, instance->vhdx_path);
    if (FAILED(hr)) {
        hcs_resume_vm(instance);
        return hr;
    }

    /* 4. Record snapshot metadata */
    wcscpy_s(snap->name, 128, snapshot_name);
    wcscpy_s(snap->state_file, MAX_PATH, state_path);
    wcscpy_s(snap->diff_vhdx, MAX_PATH, diff_path);
    wcscpy_s(snap->parent_vhdx, MAX_PATH, instance->vhdx_path);
    GetSystemTimeAsFileTime(&snap->timestamp);
    snap->valid = TRUE;

    /* 5. Update the VM's active VHDX to the differencing disk */
    wcscpy_s(instance->vhdx_path, MAX_PATH, diff_path);

    list->count++;

    /* 6. Resume the VM */
    hr = hcs_resume_vm(instance);
    return hr;
}

HRESULT snapshot_revert(SnapshotList *list, VmInstance *instance, int snapshot_index)
{
    Snapshot *snap;
    int i;

    if (!list || !instance)
        return E_INVALIDARG;

    if (snapshot_index < 0 || snapshot_index >= list->count)
        return E_INVALIDARG;

    snap = &list->snapshots[snapshot_index];
    if (!snap->valid)
        return E_NOT_VALID_STATE;

    /* 1. Stop the VM if running */
    if (instance->running) {
        hcs_terminate_vm(instance);
    }
    hcs_close_vm(instance);

    /* 2. Delete differencing VHDXs created after the target snapshot */
    for (i = list->count - 1; i > snapshot_index; i--) {
        if (list->snapshots[i].valid) {
            DeleteFileW(list->snapshots[i].diff_vhdx);
            DeleteFileW(list->snapshots[i].state_file);
            list->snapshots[i].valid = FALSE;
        }
    }
    list->count = snapshot_index + 1;

    /* 3. Restore the VHDX path to the snapshot's differencing disk */
    wcscpy_s(instance->vhdx_path, MAX_PATH, snap->diff_vhdx);

    /* VM is now stopped and ready to be re-created/started.
       The caller should re-create the VM with hcs_create_vm and then start it.
       The saved state file is at snap->state_file if the caller wants to
       restore from saved state (warm start). */

    return S_OK;
}

HRESULT snapshot_delete(SnapshotList *list, int snapshot_index)
{
    Snapshot *snap;
    HRESULT hr;
    int i;

    if (!list)
        return E_INVALIDARG;

    if (snapshot_index < 0 || snapshot_index >= list->count)
        return E_INVALIDARG;

    snap = &list->snapshots[snapshot_index];
    if (!snap->valid)
        return E_NOT_VALID_STATE;

    /* If this is the oldest snapshot, merge into parent */
    if (snapshot_index == 0) {
        hr = vhdx_merge(snap->diff_vhdx);
        if (FAILED(hr)) return hr;
        DeleteFileW(snap->diff_vhdx);
    } else {
        /* Middle snapshot: just delete the files.
           The next snapshot's parent still references the one before this. */
        DeleteFileW(snap->diff_vhdx);
    }

    /* Delete saved state file */
    DeleteFileW(snap->state_file);

    /* Shift remaining snapshots down */
    snap->valid = FALSE;
    for (i = snapshot_index; i < list->count - 1; i++) {
        list->snapshots[i] = list->snapshots[i + 1];
    }
    ZeroMemory(&list->snapshots[list->count - 1], sizeof(Snapshot));
    list->count--;

    return S_OK;
}
