#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <windows.h>
#include "hcs_vm.h"

#define MAX_SNAPSHOTS 64

typedef struct {
    wchar_t  name[128];
    wchar_t  state_file[MAX_PATH];   /* saved VM memory/device state */
    wchar_t  diff_vhdx[MAX_PATH];    /* differencing VHDX created at this snapshot */
    wchar_t  parent_vhdx[MAX_PATH];  /* the VHDX that was active before this snapshot */
    FILETIME timestamp;
    BOOL     valid;
} Snapshot;

typedef struct {
    Snapshot snapshots[MAX_SNAPSHOTS];
    int      count;
    wchar_t  base_dir[MAX_PATH]; /* directory for snapshot files */
} SnapshotList;

/* Initialize snapshot list for a VM.
   base_dir: directory where snapshot files will be stored. */
void snapshot_init(SnapshotList *list, const wchar_t *base_dir);

/* Take a snapshot of a running VM.
   Pauses the VM, saves state, creates differencing VHDX, resumes.
   snapshot_name: user-given name for this snapshot. */
HRESULT snapshot_take(SnapshotList *list, VmInstance *instance, const wchar_t *snapshot_name);

/* Revert a VM to a specific snapshot.
   Stops the VM, restores VHDX chain, prepares for restart with saved state. */
HRESULT snapshot_revert(SnapshotList *list, VmInstance *instance, int snapshot_index);

/* Delete a snapshot.
   If it's the oldest, merges differencing VHDX into parent. */
HRESULT snapshot_delete(SnapshotList *list, int snapshot_index);

#endif /* SNAPSHOT_H */
