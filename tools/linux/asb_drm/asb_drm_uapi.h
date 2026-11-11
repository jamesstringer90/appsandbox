#ifndef ASB_DRM_UAPI_H
#define ASB_DRM_UAPI_H

#ifdef __KERNEL__
#include <drm/drm.h>
#else
#include <libdrm/drm.h>
#endif

#define ASB_DRM_GPU_VERSION 1
#define ASB_DRM_GPU_CAP_BUFFER_IMPORT 1
#define ASB_DRM_GPU_CAP_NATIVE_HANDLES 2
#define ASB_DRM_GPU_CAP_SYNC_FILE 4
#define ASB_DRM_GPU_CAP_PRESENT 8
#define ASB_DRM_GPU_CAP_SCANOUT 16
#define ASB_DRM_GPU_CAP_RESIZE 32
#define ASB_DRM_GPU_CAP_RGB_FORMATS 64
#define ASB_DRM_GPU_CAP_MODIFIERS 128
#define ASB_DRM_GPU_EXPORT_PRIME_FD 1

#ifndef ASB_DRM_FORMAT_MOD_D3D12
#define ASB_DRM_FORMAT_MOD_D3D12 ((0xa5ULL << 56) | 1)
#endif

#define ASB_DRM_DISPLAY_GET 0
#define ASB_DRM_DISPLAY_SET 1
#define ASB_DRM_DISPLAY_SHARED 1
#define ASB_DRM_DISPLAY_MATCH_WINDOW 2

struct drm_asb_display_config {
	__u32 operation;
	__u32 flags;
	__u32 width;
	__u32 height;
	__u32 refresh;
};

struct drm_asb_gpu_query {
	__u32 version;
	__u32 capabilities;
	__u32 max_width;
	__u32 max_height;
	__aligned_u64 adapter_luid;
};

struct drm_asb_gpu_import {
	__s32 fd;
	__u32 handle;
	__u32 width;
	__u32 height;
	__u32 format;
	__u32 pitch;
	__aligned_u64 adapter_luid;
	__aligned_u64 flags;
};

struct drm_asb_gpu_export {
	__u32 handle;
	__u32 flags;
	__s32 fd;
	__u32 width;
	__u32 height;
	__u32 format;
	__u32 pitch;
	__u32 reserved;
	__aligned_u64 adapter_luid;
};

struct drm_asb_gpu_native {
	__s32 dxg_fd;
	__s32 fd;
	__u32 device_handle;
	__u32 object_handle;
	__aligned_u64 adapter_luid;
	__aligned_u64 flags;
};

struct drm_asb_gpu_present {
	__s32 event_fd;
	__s32 dxg_fd;
	__s32 fence_fd;
	__u32 flags;
	__aligned_u64 adapter_luid;
};

struct drm_asb_gpu_acquire {
	__s32 complete_fd;
	__s32 fd;
	__u32 width;
	__u32 height;
	__u32 format;
	__u32 pitch;
	__aligned_u64 sequence;
	__aligned_u64 adapter_luid;
	__aligned_u64 flags;
};

#define DRM_ASB_GPU_QUERY 0x00
#define DRM_ASB_GPU_IMPORT 0x01
#define DRM_ASB_GPU_EXPORT 0x02
#define DRM_ASB_GPU_NATIVE 0x03
#define DRM_ASB_GPU_PRESENT 0x05
#define DRM_ASB_GPU_ACQUIRE 0x06
#define DRM_ASB_DISPLAY_CONFIG 0x07

#define DRM_IOCTL_ASB_GPU_QUERY \
	DRM_IOR(DRM_COMMAND_BASE + DRM_ASB_GPU_QUERY, struct drm_asb_gpu_query)
#define DRM_IOCTL_ASB_GPU_IMPORT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ASB_GPU_IMPORT, struct drm_asb_gpu_import)
#define DRM_IOCTL_ASB_GPU_EXPORT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ASB_GPU_EXPORT, struct drm_asb_gpu_export)
#define DRM_IOCTL_ASB_GPU_NATIVE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ASB_GPU_NATIVE, struct drm_asb_gpu_native)
#define DRM_IOCTL_ASB_GPU_PRESENT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ASB_GPU_PRESENT, struct drm_asb_gpu_present)
#define DRM_IOCTL_ASB_GPU_ACQUIRE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ASB_GPU_ACQUIRE, struct drm_asb_gpu_acquire)
#define DRM_IOCTL_ASB_DISPLAY_CONFIG \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ASB_DISPLAY_CONFIG, struct drm_asb_display_config)

#endif
