#ifndef GPU_ENUM_H
#define GPU_ENUM_H

#include <windows.h>

#define MAX_GPUS 16
#define MAX_GPU_SHARES 64

/* A Plan9 share mapping: host directory -> guest destination */
typedef struct {
    wchar_t share_name[128];      /* Plan9 share name (e.g. "AppSandbox.Drv.0") */
    wchar_t host_path[MAX_PATH];  /* Host directory to share */
    wchar_t guest_path[MAX_PATH]; /* Where agent p9copy copies to on guest */
    wchar_t file_filter[4096];    /* Semicolon-separated filenames (empty = copy all) */
} GpuDriverShare;

typedef struct {
    GpuDriverShare shares[MAX_GPU_SHARES];
    int count;
} GpuDriverShareList;

typedef struct {
    wchar_t name[256];
    wchar_t instance_path[512];
    wchar_t interface_path[512]; /* GPU-PV partition adapter device interface path */
    wchar_t driver_store_path[MAX_PATH]; /* DriverStore\FileRepository\<folder> path */
    wchar_t service[128];       /* Kernel driver service name (e.g. "nvlddmkm") */
} GpuInfo;

typedef struct {
    GpuInfo gpus[MAX_GPUS];
    int     count;
    GpuDriverShareList shares; /* Driver directories (populated by gpu_enumerate) */
} GpuList;

/* Enumerate GPU-PV capable GPUs via SetupAPI.
   Populates list with device names, instance paths, interface paths,
   DriverStore paths, and Plan9 share mappings.
   Returns TRUE on success. */
BOOL gpu_enumerate(GpuList *list);

/* Get the DriverStore folder path for the default GPU-PV capable GPU.
   Returns TRUE and fills out_path on success.
   out_folder receives just the folder name (e.g. "nvltsi.inf_amd64_xxx"). */
BOOL gpu_get_default_driver_path(GpuList *list,
    wchar_t *out_path, int path_max,
    wchar_t *out_folder, int folder_max);

/* Copy driver shares from gpu_list into out.
   Returns TRUE if shares are available. */
BOOL gpu_get_driver_shares(GpuList *gpu_list, GpuDriverShareList *out);

#endif /* GPU_ENUM_H */
