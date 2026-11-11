// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * AppSandbox virtual DRM driver — primary + cursor planes.
 *
 * Both planes are "virtual": we don't scan anything out, we just hold the
 * compositor's framebuffer reference and signal vblank completion. The
 * appsandbox-display userland daemon discovers what the planes currently
 * point at (via drmModeGetPlane / drmModeGetFB2 / drmPrimeHandleToFD)
 * and ships the pixels over vsock.
 *
 * Format support:
 *   Primary — XRGB8888 + ARGB8888 + ARGB2101010 (matches what Mutter
 *             negotiates with Mesa LLVMpipe today; ARGB2101010 covers
 *             10-bit-per-channel paths that newer GNOME versions use).
 *   Cursor  — ARGB8888 only, max ASB_CURSOR_MAX_W x ASB_CURSOR_MAX_H.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>

#include "asb_drm.h"
#include "asb_drm_uapi.h"

/* --------------------------------------------------------------------------
 * Format tables. Listed in preference order; first format the compositor
 * accepts is what it'll use.
 * -------------------------------------------------------------------------- */

static const u32 asb_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	/* AR30 (ARGB2101010) deliberately omitted for now: the host-side
	 * AppSandbox renderer is BGRA8888-only, and Mutter picks the
	 * highest-precision format we advertise. Re-enable when the host
	 * IDD renderer learns 10-bit. */
};

static const u32 asb_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,    /* Mutter checks both alpha and no-alpha
	                         * variants; without XRGB8888 the cursor
	                         * plane suitability check rejects us. */
};

static const u64 asb_primary_modifiers[] = {
	ASB_DRM_FORMAT_MOD_D3D12,
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static const u64 asb_cursor_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static bool asb_primary_format_mod_supported(struct drm_plane *plane,
                                             u32 format, u64 modifier)
{
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return true;
	return modifier == ASB_DRM_FORMAT_MOD_D3D12 &&
	       (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888);
}

/* --------------------------------------------------------------------------
 * Primary plane
 *
 * We use drm_atomic_helper_check_plane_state with NO scaling and CRTC-sized
 * source rect. That gives userspace clear feedback if it tries to enable a
 * mismatched mode (e.g. requesting 4K on our default 1080p configuration).
 * -------------------------------------------------------------------------- */

static int asb_primary_atomic_check(struct drm_plane *plane,
                                    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state  *crtc_state;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
	                                           DRM_PLANE_NO_SCALING,
	                                           DRM_PLANE_NO_SCALING,
	                                           false, true);
}

static void asb_primary_atomic_update(struct drm_plane *plane,
                                      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	asb_gpu_update(to_asb(plane->dev), new_state->crtc ? new_state->fb : NULL);
}

static const struct drm_plane_helper_funcs asb_primary_helper_funcs = {
	.prepare_fb    = drm_gem_plane_helper_prepare_fb,
	.atomic_check  = asb_primary_atomic_check,
	.atomic_update = asb_primary_atomic_update,
};

static const struct drm_plane_funcs asb_primary_funcs = {
	.update_plane           = drm_atomic_helper_update_plane,
	.disable_plane          = drm_atomic_helper_disable_plane,
	.destroy                = drm_plane_cleanup,
	.reset                  = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_plane_destroy_state,
	.format_mod_supported   = asb_primary_format_mod_supported,
};

/* --------------------------------------------------------------------------
 * Cursor plane
 *
 * Hardware-cursor semantics: separate plane from the primary so Mutter can
 * update cursor position without redrawing the desktop. The daemon reads
 * the cursor plane's CRTC_X / CRTC_Y on every vblank to know where the
 * cursor should be drawn on the host side, and reads fb_id to detect
 * shape changes (cache by fb handle, only re-send shape when handle changes).
 *
 * We bound cursor size at ASB_CURSOR_MAX_W x ASB_CURSOR_MAX_H to match
 * mode_config.cursor_width/height; libdrm clamps userspace requests against
 * those limits.
 * -------------------------------------------------------------------------- */

static int asb_cursor_atomic_check(struct drm_plane *plane,
                                   struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc_state  *crtc_state;

	if (!new_state->crtc || !fb)
		return 0;

	if (fb->width  > ASB_CURSOR_MAX_W ||
	    fb->height > ASB_CURSOR_MAX_H) {
		drm_dbg_kms(plane->dev, "cursor too large: %ux%u (max %ux%u)\n",
		            fb->width, fb->height,
		            ASB_CURSOR_MAX_W, ASB_CURSOR_MAX_H);
		return -EINVAL;
	}

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	/* Cursor plane: scaling not allowed, but the source rect can be
	 * smaller than the CRTC. */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
	                                           DRM_PLANE_NO_SCALING,
	                                           DRM_PLANE_NO_SCALING,
	                                           true, true);
}

static void asb_cursor_atomic_update(struct drm_plane *plane,
                                     struct drm_atomic_state *state)
{
	/* The plane state already carries fb + crtc_x + crtc_y. The daemon
	 * reads those via drmModeGetPlane on the next vblank — no driver
	 * state needs to be stashed elsewhere. */
	(void)plane;
	(void)state;
}

static const struct drm_plane_helper_funcs asb_cursor_helper_funcs = {
	.prepare_fb    = drm_gem_plane_helper_prepare_fb,
	.atomic_check  = asb_cursor_atomic_check,
	.atomic_update = asb_cursor_atomic_update,
};

static const struct drm_plane_funcs asb_cursor_funcs = {
	.update_plane           = drm_atomic_helper_update_plane,
	.disable_plane          = drm_atomic_helper_disable_plane,
	.destroy                = drm_plane_cleanup,
	.reset                  = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_plane_destroy_state,
};

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */

int asb_planes_init(struct asb_device *asb)
{
	struct drm_device *drm = &asb->drm;
	int ret;

	ret = drm_universal_plane_init(drm,
	                               &asb->primary_plane,
	                               1 << 0,                     /* possible_crtcs — bit 0 = our single CRTC */
	                               &asb_primary_funcs,
	                               asb_primary_formats,
	                               ARRAY_SIZE(asb_primary_formats),
	                               asb_primary_modifiers,
	                               DRM_PLANE_TYPE_PRIMARY,
	                               "primary");
	if (ret)
		return ret;
	drm_plane_helper_add(&asb->primary_plane, &asb_primary_helper_funcs);

	ret = drm_universal_plane_init(drm,
	                               &asb->cursor_plane,
	                               1 << 0,
	                               &asb_cursor_funcs,
	                               asb_cursor_formats,
	                               ARRAY_SIZE(asb_cursor_formats),
	                               asb_cursor_modifiers,
	                               DRM_PLANE_TYPE_CURSOR,
	                               "cursor");
	if (ret)
		return ret;
	drm_plane_helper_add(&asb->cursor_plane, &asb_cursor_helper_funcs);

	/* Cursor plane sits above the primary plane. Mutter doesn't strictly
	 * require this — atomic blend mode is implicit cursor-on-top — but
	 * being explicit makes the userland daemon's read-and-overlay job
	 * unambiguous when it walks plane state. */
	drm_plane_create_zpos_immutable_property(&asb->cursor_plane, 1);
	drm_plane_create_zpos_immutable_property(&asb->primary_plane, 0);

	/* HOTSPOT_X / HOTSPOT_Y properties are auto-attached by the DRM core
	 * to cursor planes when the driver advertises DRIVER_CURSOR_HOTSPOT
	 * in its feature flags (see asb_drm_drv.c). No per-plane setup call
	 * needed in this kernel API. */

	return 0;
}
