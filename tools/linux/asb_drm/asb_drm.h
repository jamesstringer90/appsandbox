/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * AppSandbox virtual DRM driver — shared header.
 *
 * Single-display "virtual GPU" for VM guests where the host reads the
 * compositor's framebuffer over a side channel (HV-socket / vsock) instead
 * of letting the kernel scan it out to real hardware. Matches the Windows
 * AppSandboxVDD model so the host display stack stays portable across
 * Windows and Linux guests.
 *
 * Topology: one platform device → one drm_device → one CRTC → one VIRTUAL
 * connector for the compositor + one WRITEBACK connector for the daemon's
 * composited-output read path. Three planes: primary, cursor, overlay.
 */

#ifndef _ASB_DRM_H_
#define _ASB_DRM_H_

#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#include <drm/drm_device.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>
#include <drm/drm_plane.h>
#include <drm/drm_writeback.h>

/* --------------------------------------------------------------------------
 * Tunables. Module params override these at load time where applicable.
 * -------------------------------------------------------------------------- */

#define ASB_DEFAULT_WIDTH     1920
#define ASB_DEFAULT_HEIGHT    1080
#define ASB_DEFAULT_REFRESH   60

#define ASB_MAX_WIDTH         7680
#define ASB_MAX_HEIGHT        4320

#define ASB_CURSOR_MAX_W      256
#define ASB_CURSOR_MAX_H      256

/* EDID is a fixed-size 128-byte block (no extension blocks). */
#define ASB_EDID_LEN          128

/* --------------------------------------------------------------------------
 * Per-device state. Embedded into drm_device via drm_dev_alloc<>().
 * -------------------------------------------------------------------------- */

struct asb_device {
	struct drm_device       drm;
	struct mutex           display_lock;
	struct drm_file       *display_owner;
	u32                    display_flags;
	u32                    display_width;
	u32                    display_height;
	struct mutex           gpu_lock;
	struct list_head       gpu_objects;
	struct drm_file       *gpu_presenter;
	struct eventfd_ctx    *gpu_event;
	struct drm_framebuffer *gpu_scanout;
	u64                    gpu_sequence;
	u64                    gpu_adapter_luid;

	/* KMS objects — single-CRTC topology. */
	struct drm_crtc         crtc;
	struct drm_encoder      encoder;
	struct drm_connector    connector;          /* DRM_MODE_CONNECTOR_VIRTUAL */
	struct drm_writeback_connector wb_connector;

	struct drm_plane        primary_plane;
	struct drm_plane        cursor_plane;

	/* Active mode at probe time; module params seed this. */
	unsigned int            width;
	unsigned int            height;
	unsigned int            refresh;

	/* Synthesized 128-byte EDID. Built once at probe, served via
	 * drm_connector_attach_edid_property() → drm_connector_update_edid_property(). */
	u8                      edid[ASB_EDID_LEN];

	/* vblank pacing. A single hrtimer fires at `refresh` Hz and signals
	 * vblank to the CRTC. We're a virtual device so we have no real
	 * scanout to sync to; hrtimer cadence is the only timing we expose. */
	struct hrtimer          vblank_timer;
	ktime_t                 vblank_period;
	bool                    vblank_enabled;
};

static inline struct asb_device *to_asb(struct drm_device *drm)
{
	return container_of(drm, struct asb_device, drm);
}

static inline struct asb_device *crtc_to_asb(struct drm_crtc *crtc)
{
	return container_of(crtc, struct asb_device, crtc);
}

/* --------------------------------------------------------------------------
 * Subsystem init / fini — called from probe() in asb_drm_drv.c.
 * Each takes a fully-allocated asb_device and adds its KMS objects to it.
 * -------------------------------------------------------------------------- */

int  asb_mode_init(struct asb_device *asb);
void asb_mode_fini(struct asb_device *asb);

int  asb_connector_init(struct asb_device *asb);
int asb_display_config_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
void asb_display_release(struct asb_device *asb, struct drm_file *file);

int  asb_planes_init(struct asb_device *asb);

int  asb_writeback_init(struct asb_device *asb);

int asb_gpu_query_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int asb_gpu_import_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int asb_gpu_export_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int asb_gpu_native_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int asb_gpu_present_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
int asb_gpu_acquire_ioctl(struct drm_device *drm, void *data, struct drm_file *file);
void asb_gpu_postclose(struct drm_device *drm, struct drm_file *file);
void asb_gpu_update(struct asb_device *asb, struct drm_framebuffer *fb);
void asb_gpu_fini(struct asb_device *asb);
struct drm_framebuffer *asb_fb_create(struct drm_device *drm,
				    struct drm_file *file,
				    const struct drm_format_info *info,
				    const struct drm_mode_fb_cmd2 *cmd);

/* Build the synthesized EDID into asb->edid. Called from connector_init. */
void asb_build_edid(struct asb_device *asb);

#endif /* _ASB_DRM_H_ */
