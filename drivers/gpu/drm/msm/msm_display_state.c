// SPDX-License-Identifier: GPL-2.0
/*
 * Simple display state tracker
 *
 * Copyright (C) 2019 Danny Lin <danny@kdrag0n.dev>.
 */

#define pr_fmt(fmt) "display_state: " fmt

#include <linux/moduleparam.h>
#include <linux/msm_drm_notify.h>

static bool display_on __read_mostly = true;
module_param(display_on, bool, 0444);

bool is_display_on(void)
{
	return display_on;
}

static int msm_drm_notifier_cb(struct notifier_block *nb,
			       unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	unsigned int blank;

	if (event != MSM_DRM_EVENT_BLANK && event != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_DONE;

	if (!evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = *(unsigned int *)evdata->data;

	switch (blank) {
	case MSM_DRM_BLANK_POWERDOWN:
	case MSM_DRM_BLANK_LP:
		if (event == MSM_DRM_EARLY_EVENT_BLANK)
			display_on = false;
		break;
	case MSM_DRM_BLANK_UNBLANK:
		if (event == MSM_DRM_EVENT_BLANK)
			display_on = true;
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block display_state_nb __ro_after_init = {
	.notifier_call = msm_drm_notifier_cb,
	.priority = INT_MAX - 2,
};

static int __init display_state_init(void)
{
	int ret;

	ret = msm_drm_register_client(&display_state_nb);
	if (ret)
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);

	return ret;
}
late_initcall(display_state_init);
