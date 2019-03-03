/*
 * Suspend state tracker driver
 *
 * Copyright (c) 2013-2017, Pranav Vashi <neobuddy89@gmail.com>
 *           (c) 2017, Joe Maples <joe@frap129.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/msm_drm_notify.h>

bool state_suspended;

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
			state_suspended = true;
		break;
	case MSM_DRM_BLANK_UNBLANK:
		if (event == MSM_DRM_EVENT_BLANK)
			state_suspended = false;
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block display_state_nb __ro_after_init = {
	.notifier_call = msm_drm_notifier_cb,
};

static int __init state_notifier_init(void)
{
	int ret;

	ret = msm_drm_register_client(&display_state_nb);
	if (ret)
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);

	return ret;
}
late_initcall(state_notifier_init);

MODULE_AUTHOR("Pranav Vashi <neobuddy89@gmail.com>");
MODULE_DESCRIPTION("Suspend state tracker");
MODULE_LICENSE("GPLv2");
