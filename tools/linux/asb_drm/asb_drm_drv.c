// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * AppSandbox virtual DRM driver — module entry point.
 *
 * Lifecycle:
 *   module_init() registers a platform_driver and creates a single
 *   platform_device. Match triggers probe() which builds the DRM device,
 *   adds the CRTC / connector / planes via the per-subsystem init helpers,
 *   and registers with the DRM core. /dev/dri/cardN appears at that point.
 *
 * We deliberately use platform_bus (not faux_bus) because some Wayland
 * compositors filter virtual KMS devices that live under /devices/faux/.
 * The platform bus puts us at /devices/platform/asb_drm.0/drm/cardN, which
 * is indistinguishable from any other in-tree platform-bus DRM driver from
 * userspace's point of view.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "asb_drm.h"
#include "asb_drm_uapi.h"

/* --------------------------------------------------------------------------
 * Module parameters
 * -------------------------------------------------------------------------- */

static unsigned int width_param   = ASB_DEFAULT_WIDTH;
static unsigned int height_param  = ASB_DEFAULT_HEIGHT;
static unsigned int refresh_param = ASB_DEFAULT_REFRESH;

module_param_named(width,   width_param,   uint, 0444);
MODULE_PARM_DESC(width,   "Initial display width  (default 1920)");
module_param_named(height,  height_param,  uint, 0444);
MODULE_PARM_DESC(height,  "Initial display height (default 1080)");
module_param_named(refresh, refresh_param, uint, 0444);
MODULE_PARM_DESC(refresh, "Refresh rate in Hz     (default 60)");

/* --------------------------------------------------------------------------
 * drm_driver
 * -------------------------------------------------------------------------- */

DEFINE_DRM_GEM_FOPS(asb_fops);

static const struct drm_ioctl_desc asb_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ASB_GPU_QUERY, asb_gpu_query_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ASB_GPU_IMPORT, asb_gpu_import_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ASB_GPU_EXPORT, asb_gpu_export_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ASB_GPU_NATIVE, asb_gpu_native_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ASB_GPU_PRESENT, asb_gpu_present_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ASB_GPU_ACQUIRE, asb_gpu_acquire_ioctl, 0),
};

static const struct drm_driver asb_drm_driver = {
	/* We deliberately do NOT set DRIVER_CURSOR_HOTSPOT. That flag tells
	 * the kernel to expose HOTSPOT_X/HOTSPOT_Y properties on the cursor
	 * plane and to hide the cursor plane from any DRM client that hasn't
	 * opted in via DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT. Mutter only sets
	 * that client cap for a hardcoded allowlist of driver names —
	 * "qxl", "vboxvideo", "virtio_gpu", "vmwgfx" — so if we advertise
	 * the flag, Mutter never sees our cursor plane and falls back to
	 * software cursor (rendered into the primary framebuffer). Without
	 * the flag, Mutter handles hotspot itself: it pre-adjusts the
	 * cursor plane's CRTC_X/CRTC_Y by -hotspot before the atomic commit.
	 * Our daemon reads the pre-adjusted coordinates directly, which is
	 * exactly what the host renderer expects.
	 *
	 * DRIVER_RENDER exposes a render-node (/dev/dri/renderDN) and lets
	 * Mutter create a gbm_device for buffer allocation on our fd. Mutter
	 * tries the GBM-allocated cursor path first; with no Mesa driver
	 * the GBM cursor check fails, but Mutter retries with NULL gbm_device
	 * which only checks that a cursor plane advertises the format. */
	.driver_features    = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
	                      DRIVER_RENDER,

	.name               = "asb_drm",
	.desc               = "AppSandbox virtual display",
	.major              = 1,
	.minor              = 1,

	.fops               = &asb_fops,
	.ioctls             = asb_ioctls,
	.num_ioctls         = ARRAY_SIZE(asb_ioctls),
	.postclose          = asb_gpu_postclose,

	DRM_GEM_SHMEM_DRIVER_OPS,
};

/* --------------------------------------------------------------------------
 * mode_config — top-level limits and atomic-helper callbacks
 * -------------------------------------------------------------------------- */

static const struct drm_mode_config_funcs asb_mode_config_funcs = {
	.fb_create     = asb_fb_create,
	.atomic_check  = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int asb_mode_config_setup(struct asb_device *asb)
{
	struct drm_device *drm = &asb->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width   = 64;
	drm->mode_config.min_height  = 64;
	drm->mode_config.max_width   = ASB_MAX_WIDTH;
	drm->mode_config.max_height  = ASB_MAX_HEIGHT;
	drm->mode_config.cursor_width  = ASB_CURSOR_MAX_W;
	drm->mode_config.cursor_height = ASB_CURSOR_MAX_H;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.prefer_shadow   = 0;
	drm->mode_config.funcs = &asb_mode_config_funcs;
	return 0;
}

/* --------------------------------------------------------------------------
 * platform_driver — probe / remove
 * -------------------------------------------------------------------------- */

static int asb_probe(struct platform_device *pdev)
{
	struct asb_device *asb;
	struct drm_device *drm;
	int ret;

	/* simpledrm displacement is handled by a userland systemd unit
	 * (asb-evict-simpledrm.service) that runs before display-manager
	 * and unbinds the simple-framebuffer platform driver. Tried the
	 * kernel-side aperture_remove_all_conflicting_devices() route first
	 * but it doesn't evict simpledrm on this kernel because simpledrm
	 * doesn't register through the aperture registry — its platform
	 * device is created by sysfb during early boot and stays bound. */

	asb = devm_drm_dev_alloc(&pdev->dev, &asb_drm_driver,
	                         struct asb_device, drm);
	if (IS_ERR(asb))
		return PTR_ERR(asb);

	drm = &asb->drm;
	mutex_init(&asb->gpu_lock);
	INIT_LIST_HEAD(&asb->gpu_objects);
	platform_set_drvdata(pdev, drm);

	/* Clamp module params into the supported range. */
	asb->width   = clamp(width_param,   64u, (unsigned)ASB_MAX_WIDTH);
	asb->height  = clamp(height_param,  64u, (unsigned)ASB_MAX_HEIGHT);
	asb->refresh = clamp(refresh_param, 24u, 240u);

	ret = asb_mode_config_setup(asb);
	if (ret) {
		dev_err(&pdev->dev, "mode_config init failed: %d\n", ret);
		return ret;
	}

	/* Plane init MUST happen before CRTC init: drm_crtc_init_with_planes
	 * takes the primary plane as a constructor argument. */
	ret = asb_planes_init(asb);
	if (ret) {
		dev_err(&pdev->dev, "planes init failed: %d\n", ret);
		return ret;
	}

	ret = asb_mode_init(asb);
	if (ret) {
		dev_err(&pdev->dev, "mode init failed: %d\n", ret);
		return ret;
	}

	ret = asb_connector_init(asb);
	if (ret) {
		dev_err(&pdev->dev, "connector init failed: %d\n", ret);
		return ret;
	}

	ret = asb_writeback_init(asb);
	if (ret) {
		/* Writeback is optional — log and continue. */
		dev_warn(&pdev->dev, "writeback init failed (%d), continuing without\n", ret);
	}

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(&pdev->dev, "drm_dev_register failed: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "AppSandbox virtual display ready: %ux%u@%uHz\n",
	         asb->width, asb->height, asb->refresh);
	return 0;
}

static void asb_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct asb_device *asb = to_asb(drm);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
	asb_mode_fini(asb);
}

static struct platform_driver asb_platform_driver = {
	.driver = {
		.name = "asb_drm",
	},
	.probe  = asb_probe,
	.remove = asb_remove,
};

/* --------------------------------------------------------------------------
 * Module init / exit. Manually create one platform_device so the driver
 * has something to bind to (we're not enumerated by ACPI/DT/PCI/anything).
 * -------------------------------------------------------------------------- */

static struct platform_device *asb_platform_device;

static int __init asb_drm_init(void)
{
	int ret;

	ret = platform_driver_register(&asb_platform_driver);
	if (ret)
		return ret;

	asb_platform_device = platform_device_register_simple("asb_drm", 0, NULL, 0);
	if (IS_ERR(asb_platform_device)) {
		platform_driver_unregister(&asb_platform_driver);
		return PTR_ERR(asb_platform_device);
	}
	return 0;
}

static void __exit asb_drm_exit(void)
{
	platform_device_unregister(asb_platform_device);
	platform_driver_unregister(&asb_platform_driver);
}

module_init(asb_drm_init);
module_exit(asb_drm_exit);

MODULE_AUTHOR("AppSandbox");
MODULE_DESCRIPTION("AppSandbox virtual DRM/KMS driver");
/* Source is MIT (see SPDX header). MODULE_LICENSE must declare GPL-compat
 * for the kernel loader to resolve EXPORT_SYMBOL_GPL DRM helpers. */
MODULE_LICENSE("Dual MIT/GPL");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_VERSION("1.1.0");
MODULE_ALIAS("platform:asb_drm");
