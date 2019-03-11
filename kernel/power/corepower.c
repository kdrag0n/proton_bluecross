// SPDX-License-Identifier: GPL-2.0
/*
 * CorePower - System power state optimizer
 *
 * Copyright (C) 2019 Danny Lin <danny@kdrag0n.dev>.
 */

#define pr_fmt(fmt) "corepower: " fmt

#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/display_state.h>
#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/msm_drm_notify.h>
#include <linux/slab.h>
#include <soc/qcom/lpm_levels.h>

enum power_state {
	STATE_UNKNOWN,
	STATE_AWAKE,
	STATE_WAKING,
	STATE_SLEEP
};

static enum power_state next_state;
static enum power_state current_state;
static struct workqueue_struct *power_state_wq;
static DEFINE_SPINLOCK(state_lock);

static bool enabled __read_mostly = true;
static short wake_timeout __read_mostly = CONFIG_COREPOWER_WAKE_TIMEOUT;
module_param(wake_timeout, short, 0644);

static bool cpu_force_deep_idle __read_mostly = true;
static bool cluster_force_deep_idle __read_mostly = true;
static unsigned int disable_perfcl_cpus __read_mostly = 2;

/* Core */
static enum power_state set_next_state(enum power_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);
	next_state = state;
	spin_unlock_irqrestore(&state_lock, flags);

	return state;
}

static enum power_state get_current_state(void)
{
	enum power_state state;
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);
	state = current_state;
	spin_unlock_irqrestore(&state_lock, flags);

	return state;
}

static bool is_state_intensive(enum power_state state)
{
	return state != STATE_SLEEP;
}

static int update_cpu(u32 cpu, bool up)
{
	if (up && cpu_isolated(cpu))
		return sched_unisolate_cpu(cpu);
	if (!up && !cpu_isolated(cpu))
		return sched_isolate_cpu(cpu);

	return 0;
}

static int update_cpu_mask(const struct cpumask *source_mask, bool up,
			   u32 exclude_count)
{
	int ret = 0;
	u32 cpu;

	get_online_cpus();
	for_each_cpu_and(cpu, source_mask, cpu_online_mask) {
		if (exclude_count) {
			exclude_count--;
			continue;
		}

		ret = update_cpu(cpu, up);
		if (ret)
			break;
	}
	put_online_cpus();

	return ret;
}

static void state_update_worker(struct work_struct *work)
{
	enum power_state state;
	bool intensive;
	int ret;

	if (!enabled)
		goto skip_update;

	spin_lock(&state_lock);
	state = next_state;
	spin_unlock(&state_lock);
	intensive = is_state_intensive(state);

	/* Do nothing if we are already in this state, unless forced */
	if (state == current_state)
		goto skip_update;

	/* Force use of the deepest CPU idle state available */
	if (cpu_force_deep_idle) {
		get_online_cpus();
		ret = cpuidle_use_deepest_state_mask(cpu_online_mask,
						     !intensive);
		put_online_cpus();
		if (ret)
			goto skip_update;
	}

	/* Force use of the deepest CPU cluster idle state available */
	if (cluster_force_deep_idle)
		lpm_cluster_use_deepest_state(!intensive);

	/* Disable performance cluster CPUs */
	if (disable_perfcl_cpus) {
		unsigned int nr_perf_cpus = cpumask_weight(cpu_perf_mask);

		ret = update_cpu_mask(cpu_perf_mask, intensive,
				       nr_perf_cpus - disable_perfcl_cpus);
		if (ret)
			goto skip_update;
	}

skip_update:
	spin_lock(&state_lock);
	current_state = state;
	next_state = STATE_UNKNOWN;
	spin_unlock(&state_lock);
}
static DECLARE_WORK(state_update_work, state_update_worker);

static void update_state(enum power_state target_state, bool sync)
{
	set_next_state(target_state);
	queue_work(power_state_wq, &state_update_work);

	if (sync)
		flush_work(&state_update_work);
}

static void wake_reset_worker(struct work_struct *unused)
{
	flush_work(&state_update_work);

	if (get_current_state() == STATE_WAKING)
		update_state(STATE_SLEEP, false);
}
static DECLARE_DELAYED_WORK(wake_reset_work, wake_reset_worker);

void corepower_wake(void)
{
	update_state(STATE_WAKING, false);
	queue_delayed_work(power_state_wq, &wake_reset_work,
			   msecs_to_jiffies(wake_timeout));
}

/* Parameter handlers */
static int param_bool_set(const char *buf, const struct kernel_param *kp)
{
	enum power_state old_state = get_current_state();
	int ret;

	flush_work(&state_update_work);
	if (old_state != STATE_AWAKE) {
		/* Toggle state to make the change take effect */
		update_state(STATE_AWAKE, true);
		ret = param_set_bool(buf, kp);
		update_state(old_state, true);
	} else {
		ret = param_set_bool(buf, kp);
	}

	return ret;
}

static const struct kernel_param_ops bool_param_ops = {
	.set = param_bool_set,
	.get = param_get_bool
};

static int param_uint_set(const char *buf, const struct kernel_param *kp)
{
	enum power_state old_state = get_current_state();
	int ret;

	flush_work(&state_update_work);
	if (old_state != STATE_AWAKE) {
		/* Toggle state to make the change take effect */
		update_state(STATE_AWAKE, true);
		ret = param_set_uint(buf, kp);
		update_state(old_state, true);
	} else {
		ret = param_set_uint(buf, kp);
	}

	return ret;
}

static const struct kernel_param_ops uint_param_ops = {
	.set = param_uint_set,
	.get = param_get_uint
};

module_param_cb(enabled, &bool_param_ops, &enabled, 0644);
module_param_cb(cpu_force_deep_idle, &bool_param_ops, &cpu_force_deep_idle,
		0644);
module_param_cb(cluster_force_deep_idle, &bool_param_ops,
		&cluster_force_deep_idle, 0644);
module_param_cb(disable_perfcl_cpus, &uint_param_ops, &disable_perfcl_cpus,
		0644);

/* Base */
static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct msm_drm_notifier *evdata = data;
	unsigned int blank;

	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_DONE;

	if (!evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = *(unsigned int *)evdata->data;

	switch (blank) {
	case MSM_DRM_BLANK_POWERDOWN: /* Off */
	case MSM_DRM_BLANK_LP: /* AOD */
		update_state(STATE_SLEEP, false);
		break;
	case MSM_DRM_BLANK_UNBLANK: /* On */
		update_state(STATE_AWAKE, false);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block display_state_nb __ro_after_init = {
	.notifier_call = msm_drm_notifier_cb
};

static void corepower_input_event(struct input_handle *handle,
				  unsigned int type, unsigned int code,
				  int value)
{
	if (value == 1 && !is_display_on() &&
	    get_current_state() == STATE_SLEEP)
		corepower_wake();
}

static int corepower_input_connect(struct input_handler *handler,
				   struct input_dev *dev,
				   const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "corepower_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto err_unreg_handle;

	return 0;

err_unreg_handle:
	input_unregister_handle(handle);
err_free_handle:
	kfree(handle);
	return ret;
}

static void corepower_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id corepower_input_ids[] = {
	/* Power button */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) }
	},
	{ }
};

static struct input_handler corepower_input_handler = {
	.name = "corepower_handler",
	.event = corepower_input_event,
	.connect = corepower_input_connect,
	.disconnect = corepower_input_disconnect,
	.id_table = corepower_input_ids
};

static int __init corepower_init(void)
{
	int ret;

	power_state_wq =
		alloc_workqueue("corepower_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!power_state_wq)
		return -ENOMEM;

	ret = input_register_handler(&corepower_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto err_destroy_wq;
	}

	ret = msm_drm_register_client(&display_state_nb);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto err_unreg_input;
	}

	return 0;

err_unreg_input:
	input_unregister_handler(&corepower_input_handler);
err_destroy_wq:
	destroy_workqueue(power_state_wq);
	return ret;
}
late_initcall(corepower_init);
