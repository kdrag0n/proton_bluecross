// SPDX-License-Identifier: GPL-2.0
/*
 * Simple display state tracker
 *
 * Copyright (C) 2019 Danny Lin <danny@kdrag0n.dev>.
 */

#define pr_fmt(fmt) "display_state: " fmt

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/msm_drm_notify.h>
#include <linux/sysfs.h>

static struct kobject *module_kobj;
static bool display_on __read_mostly = true;

bool is_display_on(void)
{
	return display_on;
}

static ssize_t display_state_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", display_on);
}
static struct kobj_attribute display_state_attr = __ATTR_RO(display_state);

static struct attribute *attrs[] = {
	&display_state_attr.attr,
	NULL
};

static const struct attribute_group attr_group = {
	.attrs = attrs,
};

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct msm_drm_notifier *evdata = data;
	bool display_on_old;
	unsigned int blank;

	if (!evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = *(unsigned int *)evdata->data;
	display_on_old = display_on;
	display_on = blank == MSM_DRM_BLANK_UNBLANK;
	if (display_on != display_on_old)
		sysfs_notify(module_kobj, NULL, display_state_attr.attr.name);

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
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		return ret;
	}

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("Failed to locate module kobject\n");
		ret = -ENOENT;
		goto err_unreg_drm_notif;
	}

	ret = sysfs_create_group(module_kobj, &attr_group);
	if (ret) {
		pr_err("Failed to create sysfs group, err: %d\n", ret);
		goto err_put_kobj;
	}

	return 0;

err_unreg_drm_notif:
	msm_drm_unregister_client(&display_state_nb);
err_put_kobj:
	kobject_put(module_kobj);
	return ret;
}
late_initcall(display_state_init);
