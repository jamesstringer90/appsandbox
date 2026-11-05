// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * AppSandbox virtual DRM driver — CRTC + encoder + vblank pacing.
 *
 * No real scanout: the CRTC's atomic_enable/disable and atomic_flush are
 * essentially no-ops, with the single exception that we arm a page-flip
 * event in atomic_flush so userspace sees swap completion.
 *
 * vblank: we own an hrtimer running at the configured refresh rate. Each
 * fire calls drm_crtc_handle_vblank() so Mutter's swap chain unblocks at
 * a steady cadence. The timer arms when userspace calls drm_vblank_get()
 * and disarms when the last reference drops.
 */

#include <linux/math64.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "asb_drm.h"

/* --------------------------------------------------------------------------
 * vblank — hrtimer driven, fires at the configured refresh rate.
 * -------------------------------------------------------------------------- */

static enum hrtimer_restart asb_vblank_timer_fn(struct hrtimer *timer)
{
	struct asb_device *asb = container_of(timer, struct asb_device, vblank_timer);

	drm_crtc_handle_vblank(&asb->crtc);

	if (!asb->vblank_enabled)
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, asb->vblank_period);
	return HRTIMER_RESTART;
}

static int asb_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct asb_device *asb = crtc_to_asb(crtc);

	asb->vblank_enabled = true;
	hrtimer_start(&asb->vblank_timer, asb->vblank_period, HRTIMER_MODE_REL);
	return 0;
}

static void asb_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct asb_device *asb = crtc_to_asb(crtc);

	asb->vblank_enabled = false;
	/* Don't cancel synchronously — the timer callback drops out on its
	 * own once vblank_enabled is false. Calling hrtimer_cancel() here
	 * would deadlock if disable_vblank is called from the same context
	 * the timer is firing in. */
}

/* --------------------------------------------------------------------------
 * CRTC atomic callbacks
 * -------------------------------------------------------------------------- */

static void asb_crtc_atomic_enable(struct drm_crtc *crtc,
                                   struct drm_atomic_state *state)
{
	struct asb_device *asb = crtc_to_asb(crtc);
	const struct drm_display_mode *mode = &crtc->state->adjusted_mode;

	asb->vblank_period = ns_to_ktime(div_u64((u64)mode->htotal * mode->vtotal * NSEC_PER_MSEC,
	                                      max(mode->clock, 1)));
	drm_crtc_vblank_on(crtc);
}

static void asb_crtc_atomic_disable(struct drm_crtc *crtc,
                                    struct drm_atomic_state *state)
{
	struct asb_device *asb = crtc_to_asb(crtc);

	drm_crtc_vblank_off(crtc);
	asb_gpu_update(asb, NULL);
	asb->vblank_enabled = false;
	hrtimer_cancel(&asb->vblank_timer);
}

static void asb_crtc_atomic_flush(struct drm_crtc *crtc,
                                  struct drm_atomic_state *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (!event)
		return;

	crtc->state->event = NULL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static const struct drm_crtc_helper_funcs asb_crtc_helper_funcs = {
	.atomic_enable  = asb_crtc_atomic_enable,
	.atomic_disable = asb_crtc_atomic_disable,
	.atomic_flush   = asb_crtc_atomic_flush,
};

static const struct drm_crtc_funcs asb_crtc_funcs = {
	.set_config             = drm_atomic_helper_set_config,
	.page_flip              = drm_atomic_helper_page_flip,
	.reset                  = drm_atomic_helper_crtc_reset,
	.destroy                = drm_crtc_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,

	.enable_vblank          = asb_crtc_enable_vblank,
	.disable_vblank         = asb_crtc_disable_vblank,
};

/* --------------------------------------------------------------------------
 * Init / fini
 * -------------------------------------------------------------------------- */

int asb_mode_init(struct asb_device *asb)
{
	struct drm_device *drm = &asb->drm;
	int ret;

	/* vblank: prepare timer and seed the period from the requested mode.
	 * Timer doesn't run until enable_vblank fires.
	 *
	 * Kernel 6.15+ replaced hrtimer_init+function-assignment with the
	 * single hrtimer_setup() entry point that takes the callback as
	 * an argument. */
	hrtimer_setup(&asb->vblank_timer, asb_vblank_timer_fn,
	              CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	asb->vblank_period = ns_to_ktime(NSEC_PER_SEC / asb->refresh);

	ret = drm_crtc_init_with_planes(drm, &asb->crtc,
	                                &asb->primary_plane,
	                                &asb->cursor_plane,
	                                &asb_crtc_funcs, "asb-crtc");
	if (ret)
		return ret;
	drm_crtc_helper_add(&asb->crtc, &asb_crtc_helper_funcs);

	ret = drm_simple_encoder_init(drm, &asb->encoder,
	                              DRM_MODE_ENCODER_VIRTUAL);
	if (ret)
		return ret;
	asb->encoder.possible_crtcs = 1 << 0;       /* our single CRTC */

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;

	return 0;
}

void asb_mode_fini(struct asb_device *asb)
{
	asb->vblank_enabled = false;
	hrtimer_cancel(&asb->vblank_timer);
	asb_gpu_fini(asb);
}
