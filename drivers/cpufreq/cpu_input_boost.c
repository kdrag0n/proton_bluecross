// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_drm_notify.h>
#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/kthread.h>

unsigned long last_input_jiffies;

static __read_mostly unsigned int input_boost_freq_lp = CONFIG_INPUT_BOOST_FREQ_LP;
static __read_mostly unsigned int input_boost_freq_hp = CONFIG_INPUT_BOOST_FREQ_PERF;
static __read_mostly unsigned int input_boost_return_freq_lp = CONFIG_REMOVE_INPUT_BOOST_FREQ_LP;
static __read_mostly unsigned int input_boost_return_freq_hp = CONFIG_REMOVE_INPUT_BOOST_FREQ_PERF;
static __read_mostly unsigned int general_boost_freq_lp = CONFIG_GENERAL_BOOST_FREQ_LP;
static __read_mostly unsigned int general_boost_freq_hp = CONFIG_GENERAL_BOOST_FREQ_PERF;
static __read_mostly unsigned short input_boost_duration = CONFIG_INPUT_BOOST_DURATION_MS;

module_param(input_boost_freq_lp, uint, 0644);
module_param(input_boost_freq_hp, uint, 0644);
module_param_named(remove_input_boost_freq_lp, input_boost_return_freq_lp, uint, 0644);
module_param_named(remove_input_boost_freq_perf, input_boost_return_freq_hp, uint, 0644);
module_param(general_boost_freq_lp, uint, 0644);
module_param(general_boost_freq_hp, uint, 0644);
module_param(input_boost_duration, short, 0644);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static __read_mostly int input_stune_boost = CONFIG_INPUT_BOOST_STUNE_LEVEL;
static __read_mostly int max_stune_boost = CONFIG_MAX_BOOST_STUNE_LEVEL;
static __read_mostly int general_stune_boost = CONFIG_GENERAL_BOOST_STUNE_LEVEL;
static __read_mostly int display_stune_boost = CONFIG_DISPLAY_BOOST_STUNE_LEVEL;

module_param_named(dynamic_stune_boost, input_stune_boost, int, 0644);
module_param(max_stune_boost, int, 0644);
module_param(general_stune_boost, int, 0644);
module_param(display_stune_boost, int, 0644);
#endif

/* Available bits for boost_drv state */
#define SCREEN_AWAKE		BIT(0)
#define INPUT_BOOST		BIT(1)
#define MAX_BOOST		BIT(2)
#define GENERAL_BOOST		BIT(3)
#define INPUT_STUNE_BOOST	BIT(4)
#define MAX_STUNE_BOOST		BIT(5)
#define GENERAL_STUNE_BOOST	BIT(6)
#define DISPLAY_STUNE_BOOST	BIT(7)

struct boost_drv {
	struct kthread_worker worker;
	struct task_struct *worker_thread;
	struct kthread_work input_boost;
	struct delayed_work input_unboost;
	struct kthread_work max_boost;
	struct delayed_work max_unboost;
	struct kthread_work general_boost;
	struct delayed_work general_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	atomic64_t max_boost_expires;
	atomic_t max_boost_dur;
	atomic64_t general_boost_expires;
	atomic_t general_boost_dur;
	atomic_t state;
	int input_stune_slot;
	int max_stune_slot;
	int general_stune_slot;
	int display_stune_slot;
};

static struct boost_drv *boost_drv_g __read_mostly;

static u32 get_boost_freq(struct boost_drv *b, u32 cpu, u32 state)
{
	if (state & INPUT_BOOST) {
		if (cpumask_test_cpu(cpu, cpu_lp_mask))
			return input_boost_freq_lp;

		return input_boost_freq_hp;
	}

	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return general_boost_freq_lp;

	return general_boost_freq_hp;
}

static u32 get_min_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return input_boost_return_freq_lp;

	return input_boost_return_freq_hp;
}

static u32 get_boost_state(struct boost_drv *b)
{
	return atomic_read(&b->state);
}

static void set_boost_bit(struct boost_drv *b, u32 state)
{
	atomic_or(state, &b->state);
}

static void clear_boost_bit(struct boost_drv *b, u32 state)
{
	atomic_andnot(state, &b->state);
}

static void update_online_cpu_policy(void)
{
	u32 cpu;

	/* Trigger cpufreq notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void update_stune_boost(struct boost_drv *b, u32 state, u32 bit, int level,
			    int *slot)
{
	if (level && !(state & bit)) {
		if (!do_stune_boost("top-app", level, slot))
			set_boost_bit(b, bit);
	}
}

static void clear_stune_boost(struct boost_drv *b, u32 state, u32 bit, int slot)
{
	if (state & bit) {
		reset_stune_boost("top-app", slot);
		clear_boost_bit(b, bit);
	}
}

static void unboost_all_cpus(struct boost_drv *b)
{
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->input_unboost) &&
		!cancel_delayed_work_sync(&b->max_unboost))
		return;

	clear_boost_bit(b, INPUT_BOOST | MAX_BOOST | GENERAL_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, INPUT_STUNE_BOOST, b->input_stune_slot);
	clear_stune_boost(b, state, MAX_STUNE_BOOST, b->max_stune_slot);
	clear_stune_boost(b, state, GENERAL_STUNE_BOOST, b->general_stune_slot);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	kthread_queue_work(&b->worker, &b->input_boost);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms)
{
	unsigned long curr_expires, new_expires;

	do {
		curr_expires = atomic64_read(&b->max_boost_expires);
		new_expires = jiffies + msecs_to_jiffies(duration_ms);

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic64_cmpxchg(&b->max_boost_expires, curr_expires,
		new_expires) != curr_expires);

	atomic_set(&b->max_boost_dur, duration_ms);
	kthread_queue_work(&b->worker, &b->max_boost);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	__cpu_input_boost_kick_max(b, duration_ms);
}

static void __cpu_input_boost_kick_general(struct boost_drv *b,
	unsigned int duration_ms)
{
	unsigned long curr_expires, new_expires;

	do {
		curr_expires = atomic64_read(&b->general_boost_expires);
		new_expires = jiffies + msecs_to_jiffies(duration_ms);

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic64_cmpxchg(&b->general_boost_expires, curr_expires,
		new_expires) != curr_expires);

	atomic_set(&b->general_boost_dur, duration_ms);
	kthread_queue_work(&b->worker, &b->general_boost);
}

void cpu_input_boost_kick_general(unsigned int duration_ms)
{
	struct boost_drv *b = boost_drv_g;
	u32 state;

	if (!b)
		return;

	state = get_boost_state(b);

	if (!(state & SCREEN_AWAKE))
		return;

	__cpu_input_boost_kick_general(b, duration_ms);
}

static void input_boost_worker(struct kthread_work *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), input_boost);
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->input_unboost)) {
		set_boost_bit(b, INPUT_BOOST);
		update_online_cpu_policy();
	}

	queue_delayed_work(system_power_efficient_wq, &b->input_unboost,
		msecs_to_jiffies(input_boost_duration));

	update_stune_boost(b, state, INPUT_STUNE_BOOST, input_stune_boost,
		&b->input_stune_slot);
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);
	u32 state = get_boost_state(b);

	clear_boost_bit(b, INPUT_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, INPUT_STUNE_BOOST, b->input_stune_slot);
}

static void max_boost_worker(struct kthread_work *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), max_boost);
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->max_unboost)) {
		set_boost_bit(b, MAX_BOOST);
		update_online_cpu_policy();
	}

	queue_delayed_work(system_power_efficient_wq, &b->max_unboost,
		msecs_to_jiffies(atomic_read(&b->max_boost_dur)));

	update_stune_boost(b, state, MAX_STUNE_BOOST, max_stune_boost,
		&b->max_stune_slot);
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);
	u32 state = get_boost_state(b);

	clear_boost_bit(b, MAX_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, MAX_STUNE_BOOST, b->max_stune_slot);
}

static void general_boost_worker(struct kthread_work *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), general_boost);
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->general_unboost)) {
		set_boost_bit(b, GENERAL_BOOST);
		update_online_cpu_policy();
	}

	queue_delayed_work(system_power_efficient_wq, &b->general_unboost,
		msecs_to_jiffies(atomic_read(&b->general_boost_dur)));

	update_stune_boost(b, state, GENERAL_STUNE_BOOST, general_stune_boost,
		&b->general_stune_slot);
}

static void general_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), general_unboost);
	u32 state = get_boost_state(b);

	clear_boost_bit(b, GENERAL_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, GENERAL_STUNE_BOOST, b->general_stune_slot);
}

static int cpu_notifier_cb(struct notifier_block *nb,
			   unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	u32 boost_freq, min_freq, state;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	state = get_boost_state(b);

	/* Boost CPU to max frequency for max boost */
	if (state & MAX_BOOST) {
		policy->min = policy->max;
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (state & INPUT_BOOST || state & GENERAL_BOOST) {
		boost_freq = get_boost_freq(b, policy->cpu, state);
		policy->min = min(policy->max, boost_freq);
	} else {
		min_freq = get_min_freq(b, policy->cpu);
		policy->min = max(policy->cpuinfo.min_freq, min_freq);
	}

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;
	u32 state = get_boost_state(b);

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		set_boost_bit(b, SCREEN_AWAKE);
		update_stune_boost(b, state, DISPLAY_STUNE_BOOST,
				   display_stune_boost, &b->display_stune_slot);
		__cpu_input_boost_kick_max(b, CONFIG_WAKE_BOOST_DURATION_MS);
#ifdef CONFIG_CPU_INPUT_BOOST_DEBUG
		pr_info("kicked max wake boost due to unblank event\n");
#endif
	} else {
		clear_boost_bit(b, SCREEN_AWAKE);
		clear_stune_boost(b, state, DISPLAY_STUNE_BOOST,
				  b->display_stune_slot);
		unboost_all_cpus(b);
#ifdef CONFIG_CPU_INPUT_BOOST_DEBUG
		pr_info("cleared all boosts due to blank event\n");
#endif
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;
	u32 state;

	state = get_boost_state(b);

	if (!(state & SCREEN_AWAKE))
		return;

	kthread_queue_work(&b->worker, &b->input_boost);

	last_input_jiffies = jiffies;
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
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
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b;
	int ret, i;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 2 };
	cpumask_t sys_bg_mask;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	kthread_init_worker(&b->worker);
	b->worker_thread = kthread_run(kthread_worker_fn, &b->worker,
				       "cpu_input_boost_thread");
	if (IS_ERR(b->worker_thread)) {
		ret = PTR_ERR(b->worker_thread);
		pr_err("Failed to start kworker, err: %d\n", ret);
		goto free_b;
	}

	ret = sched_setscheduler(b->worker_thread, SCHED_FIFO, &param);
	if (ret)
		pr_err("Failed to set SCHED_FIFO on kworker, err: %d\n", ret);

	/* Init the cpumask: 1-3 inclusive */
	for (i = 1; i <= 3; i++)
		cpumask_set_cpu(i, &sys_bg_mask);

	/* Bind it to the cpumask */
	kthread_bind_mask(b->worker_thread, &sys_bg_mask);

	/* Wake it up */
	wake_up_process(b->worker_thread);

	atomic64_set(&b->max_boost_expires, 0);
	kthread_init_work(&b->input_boost, input_boost_worker);
	INIT_DELAYED_WORK(&b->input_unboost, input_unboost_worker);
	kthread_init_work(&b->max_boost, max_boost_worker);
	INIT_DELAYED_WORK(&b->max_unboost, max_unboost_worker);
	kthread_init_work(&b->general_boost, general_boost_worker);
	INIT_DELAYED_WORK(&b->general_unboost, general_unboost_worker);
	atomic_set(&b->state, 0);

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	b->cpu_notif.priority = INT_MAX - 2;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		goto destroy_wq;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	boost_drv_g = b;

	return 0;

unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
destroy_wq:
	kthread_destroy_worker(&b->worker);
free_b:
	kfree(b);
	return ret;
}
late_initcall(cpu_input_boost_init);
