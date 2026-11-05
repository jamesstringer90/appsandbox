// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * AppSandbox virtual DRM driver — VIRTUAL connector + synthesized EDID.
 *
 * Reports a single "monitor" with manufacturer code "ASB", monitor name
 * "AppSandbox" and a preferred mode determined by module params. The mode
 * list returned by get_modes() comes from drm_cvt_mode() so userspace can
 * pick smaller resolutions if it wants.
 *
 * EDID layout follows VESA 1.4. We hardcode the structural bytes and the
 * detailed-timing descriptor; checksum byte is patched at the end.
 */

#include <linux/capability.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "asb_drm.h"
#include "asb_drm_uapi.h"

/* --------------------------------------------------------------------------
 * EDID builder
 *
 * The detailed-timing descriptor for the active mode is computed from CVT
 * (drm_cvt_mode) on the fly so module-param width/height/refresh changes
 * produce a matching EDID.
 * -------------------------------------------------------------------------- */

static void put_le16(u8 *p, u16 v) { p[0] = v & 0xff; p[1] = v >> 8; }

static void asb_fill_dtd(u8 dtd[18], const struct drm_display_mode *m)
{
	u16 clk100      = m->clock / 10;                      /* pixel clock / 10 kHz */
	u16 h_active    = m->hdisplay;
	u16 h_blank     = m->htotal - m->hdisplay;
	u16 v_active    = m->vdisplay;
	u16 v_blank     = m->vtotal - m->vdisplay;
	u16 h_sync_off  = m->hsync_start - m->hdisplay;
	u16 h_sync_w    = m->hsync_end   - m->hsync_start;
	u16 v_sync_off  = m->vsync_start - m->vdisplay;
	u16 v_sync_w    = m->vsync_end   - m->vsync_start;

	memset(dtd, 0, 18);
	put_le16(&dtd[0], clk100);
	dtd[2]  =  h_active & 0xff;
	dtd[3]  =  h_blank  & 0xff;
	dtd[4]  = ((h_active >> 8) & 0xf) << 4 | ((h_blank >> 8) & 0xf);
	dtd[5]  =  v_active & 0xff;
	dtd[6]  =  v_blank  & 0xff;
	dtd[7]  = ((v_active >> 8) & 0xf) << 4 | ((v_blank >> 8) & 0xf);
	dtd[8]  =  h_sync_off & 0xff;
	dtd[9]  =  h_sync_w   & 0xff;
	dtd[10] = ((v_sync_off & 0xf) << 4) | (v_sync_w & 0xf);
	dtd[11] = ((h_sync_off >> 8) & 0x3) << 6
	        | ((h_sync_w   >> 8) & 0x3) << 4
	        | ((v_sync_off >> 4) & 0x3) << 2
	        | ((v_sync_w   >> 4) & 0x3);
	/* Image size in mm — approximate 16:9 24-inch panel (~530 x 300). */
	dtd[12] = 530 & 0xff;
	dtd[13] = 300 & 0xff;
	dtd[14] = ((530 >> 8) & 0xf) << 4 | ((300 >> 8) & 0xf);
	/* Borders + flags: digital separate sync, +H +V polarity. */
	dtd[17] = 0x18;
}

void asb_build_edid(struct asb_device *asb)
{
	u8 *e = asb->edid;
	struct drm_display_mode m;
	int i, sum = 0;
	const char *name = "AppSandbox";

	memset(e, 0, ASB_EDID_LEN);

	/* Header (0..7) */
	e[0] = 0x00;
	for (i = 1; i <= 6; i++) e[i] = 0xff;
	e[7] = 0x00;

	/* Manufacturer "ASB" = packed 5-bit: A=1, S=19, B=2
	 *   id_msb = (A << 2) | (S >> 3) = 0x06
	 *   id_lsb = ((S & 0x7) << 5) | B = 0x62 */
	e[8]  = 0x06;
	e[9]  = 0x62;
	put_le16(&e[10], 0x0001);                       /* product code */
	put_le16(&e[12], 0x00000001 & 0xffff);          /* serial low */
	put_le16(&e[14], 0x00000001 >> 16);             /* serial high */

	e[16] = 1;          /* manufacture week */
	e[17] = 36;         /* year - 1990 → 2026 */
	e[18] = 1;          /* EDID major version */
	e[19] = 4;          /* EDID minor version */

	/* Video input: digital, 8 bpc, DisplayPort interface */
	e[20] = 0xa5;
	e[21] = 53;         /* horizontal screen size cm */
	e[22] = 30;         /* vertical screen size cm */
	e[23] = 120;        /* gamma = (gamma_real * 100) - 100 → 2.2 */
	e[24] = 0x22;       /* features: RGB, preferred timing in first DTD */

	/* Chromaticity coordinates — sRGB-ish */
	e[25] = 0xee; e[26] = 0x91; e[27] = 0xa3;
	e[28] = 0x54; e[29] = 0x4c; e[30] = 0x99;
	e[31] = 0x26; e[32] = 0x0f; e[33] = 0x50;
	e[34] = 0x54;

	/* Established timings (none — we publish via DTD/CVT) */
	e[35] = e[36] = e[37] = 0;

	/* Standard timings — all "unused" markers */
	for (i = 38; i <= 53; i++) e[i] = 0x01;

	/* DTD #1 (bytes 54..71): the preferred mode, computed from CVT. */
	{
		struct drm_display_mode *cvt = drm_cvt_mode(NULL, asb->width,
		                                            asb->height,
		                                            asb->refresh,
		                                            false, false, false);
		if (cvt) {
			m = *cvt;
			drm_mode_destroy(NULL, cvt);
			asb_fill_dtd(&e[54], &m);
		}
	}

	/* Descriptor #2 (72..89) — monitor name (0xFC) */
	e[72] = e[73] = e[74] = 0; e[75] = 0xfc; e[76] = 0;
	for (i = 0; i < (int)strlen(name) && i < 13; i++)
		e[77 + i] = name[i];
	for (; i < 13; i++) e[77 + i] = (i == strlen(name)) ? 0x0a : 0x20;

	/* Descriptor #3 (90..107) — range limits (0xFD) */
	e[90] = e[91] = e[92] = 0; e[93] = 0xfd; e[94] = 0;
	e[95] = 24;        /* min vertical Hz */
	e[96] = 75;        /* max vertical Hz */
	e[97] = 30;        /* min horizontal kHz */
	e[98] = 150;       /* max horizontal kHz */
	e[99] = 220;       /* max pixel clock / 10 MHz → 2200 MHz cap */
	e[100] = 0x0a;
	for (i = 101; i <= 107; i++) e[i] = 0x20;

	/* Descriptor #4 (108..125) — placeholder */
	e[108] = e[109] = e[110] = 0; e[111] = 0x10; e[112] = 0;
	for (i = 113; i <= 125; i++) e[i] = 0x00;

	/* Extension block count + checksum */
	e[126] = 0;
	for (i = 0; i < 127; i++) sum += e[i];
	e[127] = (u8)(0x100 - (sum & 0xff));
}

/* --------------------------------------------------------------------------
 * Connector helper callbacks
 * -------------------------------------------------------------------------- */

static int asb_connector_get_modes(struct drm_connector *connector)
{
	struct asb_device *asb = to_asb(connector->dev);
	static const struct { unsigned int width, height; } sizes[] = {
		{ 1920, 1080 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 },
		{ 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 },
		{ 1680, 1050 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
		{ 3440, 1440 }, { 3840, 2160 }, { 5120, 1440 }, { 5120, 2880 },
		{ 7680, 4320 },
	};
	struct drm_display_mode *m;
	u32 flags, width, height;
	unsigned int i, refresh;
	int count = 0;

	mutex_lock(&asb->display_lock);
	flags = asb->display_flags;
	width = asb->display_width;
	height = asb->display_height;
	refresh = asb->display_refresh;
	mutex_unlock(&asb->display_lock);

	drm_connector_update_edid_property(connector,
	                                   (const struct edid *)asb->edid);

	m = drm_cvt_mode(connector->dev, roundup(width, 8), height, refresh,
	                 false, false, false);
	if (m) {
		m->hdisplay = width;
		drm_mode_set_name(m);
		m->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(connector, m);
		count++;
	}

	if (flags & ASB_DRM_DISPLAY_MATCH_WINDOW)
		return count;
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if ((sizes[i].width == width && sizes[i].height == height && refresh == 60) ||
		    (!(flags & ASB_DRM_DISPLAY_SHARED) &&
		     (sizes[i].width > 1920 || sizes[i].height > 1080)))
			continue;
		m = drm_cvt_mode(connector->dev, roundup(sizes[i].width, 8),
		                 sizes[i].height, 60, false, false, false);
		if (m) {
			m->hdisplay = sizes[i].width;
			drm_mode_set_name(m);
			m->type |= DRM_MODE_TYPE_DRIVER;
			drm_mode_probed_add(connector, m);
			count++;
		}
	}

	return count;
}

static enum drm_mode_status asb_connector_mode_valid(struct drm_connector *connector,
                                                      const struct drm_display_mode *mode)
{
	struct asb_device *asb = to_asb(connector->dev);
	u32 flags, width, height;

	mutex_lock(&asb->display_lock);
	flags = asb->display_flags;
	width = asb->display_width;
	height = asb->display_height;
	mutex_unlock(&asb->display_lock);
	if (mode->hdisplay < 64 || mode->vdisplay < 64 ||
	    mode->hdisplay > ASB_MAX_WIDTH || mode->vdisplay > ASB_MAX_HEIGHT)
		return MODE_BAD;
	if (!(flags & ASB_DRM_DISPLAY_SHARED) &&
	    (mode->hdisplay > 1920 || mode->vdisplay > 1080 ||
	     (u64)mode->clock * 1000 > (u64)mode->htotal * mode->vtotal * 60))
		return MODE_BAD;
	if ((flags & ASB_DRM_DISPLAY_MATCH_WINDOW) &&
	    (mode->hdisplay != width || mode->vdisplay != height))
		return MODE_BAD;
	return MODE_OK;
}

int asb_display_config_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct asb_device *asb = to_asb(drm);
	struct drm_asb_display_config *args = data;
	bool changed = false;

	if (args->operation != ASB_DRM_DISPLAY_GET && args->operation != ASB_DRM_DISPLAY_SET)
		return -EINVAL;
	if (args->operation == ASB_DRM_DISPLAY_SET) {
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (!args->refresh) args->refresh = 60;
		if (args->flags & ~(ASB_DRM_DISPLAY_SHARED | ASB_DRM_DISPLAY_MATCH_WINDOW) ||
		    args->refresh < 24 || args->refresh > 1000 ||
		    (!(args->flags & ASB_DRM_DISPLAY_MATCH_WINDOW) && args->refresh != 60) ||
		    args->width < 64 || args->height < 64 ||
		    args->width > ASB_MAX_WIDTH || args->height > ASB_MAX_HEIGHT ||
		    (!(args->flags & ASB_DRM_DISPLAY_SHARED) &&
		     (args->width > 1920 || args->height > 1080 || args->refresh > 60)) ||
		    ((args->flags & ASB_DRM_DISPLAY_MATCH_WINDOW) &&
		     (!(args->flags & ASB_DRM_DISPLAY_SHARED) || args->width < 1024 || args->height < 768)))
			return -EINVAL;
	}
	mutex_lock(&asb->display_lock);
	if (args->operation == ASB_DRM_DISPLAY_SET) {
		if (asb->display_owner && asb->display_owner != file) {
			mutex_unlock(&asb->display_lock);
			return -EBUSY;
		}
		changed = asb->display_flags != args->flags ||
		          asb->display_width != args->width || asb->display_height != args->height ||
		          asb->display_refresh != args->refresh;
		asb->display_owner = file;
		asb->display_flags = args->flags;
		asb->display_width = args->width;
		asb->display_height = args->height;
		asb->display_refresh = args->refresh;
	} else {
		args->flags = asb->display_flags;
		args->width = asb->display_width;
		args->height = asb->display_height;
		args->refresh = asb->display_refresh;
	}
	mutex_unlock(&asb->display_lock);
	if (changed)
		drm_kms_helper_connector_hotplug_event(&asb->connector);
	return 0;
}

void asb_display_release(struct asb_device *asb, struct drm_file *file)
{
	bool changed = false;

	mutex_lock(&asb->display_lock);
	if (asb->display_owner == file) {
		changed = asb->display_flags != 0 || asb->display_width != min(asb->width, 1920u) ||
		          asb->display_height != min(asb->height, 1080u) || asb->display_refresh > 60;
		asb->display_owner = NULL;
		asb->display_flags = 0;
		asb->display_width = min(asb->width, 1920u);
		asb->display_height = min(asb->height, 1080u);
		asb->display_refresh = min(asb->refresh, 60u);
	}
	mutex_unlock(&asb->display_lock);
	if (changed && !drm_dev_is_unplugged(&asb->drm))
		drm_kms_helper_connector_hotplug_event(&asb->connector);
}

static const struct drm_connector_helper_funcs asb_connector_helper_funcs = {
	.get_modes = asb_connector_get_modes,
	.mode_valid = asb_connector_mode_valid,
};

static const struct drm_connector_funcs asb_connector_funcs = {
	.fill_modes             = drm_helper_probe_single_connector_modes,
	.destroy                = drm_connector_cleanup,
	.reset                  = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */

int asb_connector_init(struct asb_device *asb)
{
	struct drm_device *drm = &asb->drm;
	int ret;

	asb_build_edid(asb);

	ret = drm_connector_init(drm, &asb->connector,
	                         &asb_connector_funcs,
	                         DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;
	drm_connector_helper_add(&asb->connector, &asb_connector_helper_funcs);

	asb->connector.status = connector_status_connected;
	asb->connector.interlace_allowed = false;
	asb->connector.doublescan_allowed = false;

	ret = drm_connector_attach_encoder(&asb->connector, &asb->encoder);
	if (ret)
		return ret;

	return 0;
}
