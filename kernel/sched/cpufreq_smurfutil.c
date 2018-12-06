/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include <linux/state_notifier.h>
#include "sched.h"
#include "tune.h"

#define RATE_LIMIT				0

#define BIT_SHIFT_1 				7
#define BIT_SHIFT_1_2 				4
#define BIT_SHIFT_2 				6
#define TARGET_LOAD_1				30
#define TARGET_LOAD_2				80

#define BIT_SHIFT_1_BIGC 			8
#define BIT_SHIFT_1_2_BIGC 			4
#define BIT_SHIFT_2_BIGC 			6
#define TARGET_LOAD_1_BIGC 			30
#define TARGET_LOAD_2_BIGC 			80

#define DEFAULT_SUSPEND_MAX_FREQ_SILVER 300000
#define DEFAULT_SUSPEND_MAX_FREQ_GOLD 825600
#define DEFAULT_SUSPEND_CAPACITY_FACTOR 10

/* Stub out fast switch routines present on mainline to reduce the backport
 * overhead. */
#define cpufreq_driver_fast_switch(x, y) 0
#define cpufreq_enable_fast_switch(x)
#define cpufreq_disable_fast_switch(x)
#define LATENCY_MULTIPLIER			(1000)
#define SMUGOV_KTHREAD_PRIORITY	50

struct smugov_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	unsigned int hispeed_load;
	unsigned int hispeed_freq;
	bool pl;
	bool iowait_boost_enable;
	unsigned int bit_shift1;
	unsigned int bit_shift1_2;
	unsigned int bit_shift2;
	unsigned int target_load1;
	unsigned int target_load2;
	unsigned int silver_suspend_max_freq;
	unsigned int gold_suspend_max_freq;
	unsigned int suspend_capacity_factor;
};

struct smugov_policy {
	struct cpufreq_policy *policy;

	struct smugov_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	u64 last_ws;
	u64 curr_cycles;
	u64 last_cyc_update_time;
	unsigned long avg_cap;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	unsigned long hispeed_util;
	unsigned long max;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;

	bool need_freq_update;
};

struct smugov_cpu {
	struct update_util_data update_util;
	struct smugov_policy *sg_policy;

	unsigned long iowait_boost;
	unsigned long iowait_boost_max;
	u64 last_update;

	struct sched_walt_cpu_load walt_load;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	unsigned int flags;
	unsigned int cpu;

	/* The field below is for single-CPU policies only. */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct smugov_cpu, smugov_cpu);
static unsigned int stale_ns;
static DEFINE_PER_CPU(struct smugov_tunables *, cached_tunables);

/************************ Governor internals ***********************/

static bool smugov_should_update_freq(struct smugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		/*
		 * This happens when limits change, so forget the previous
		 * next_freq value and force an update.
		 */
		sg_policy->next_freq = UINT_MAX;
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;
	/* No need to recalculate next freq for min_rate_limit_us at least */
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool smugov_up_down_rate_limit(struct smugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;
	delta_ns = time - sg_policy->last_freq_update_time;
	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;
	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;
	return false;
}

static void smugov_update_commit(struct smugov_policy *sg_policy, u64 time,
				unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	if (smugov_up_down_rate_limit(sg_policy, time, next_freq)) {
		/* Don't cache a raw freq that didn't become next_freq */
		sg_policy->cached_raw_freq = 0;
		return;
	}
	if (sg_policy->next_freq == next_freq)
		return;

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	if (policy->fast_switch_enabled) {
		next_freq = cpufreq_driver_fast_switch(policy, next_freq);
		if (next_freq == CPUFREQ_ENTRY_INVALID)
			return;

		policy->cur = next_freq;
		trace_cpu_frequency(next_freq, smp_processor_id());
	} else {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
}

#define TARGET_LOAD 80
/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: smurfutil policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
static unsigned int get_next_freq(struct smugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{	
	struct cpufreq_policy *policy = sg_policy->policy;
	struct smugov_tunables *tunables = sg_policy->tunables;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;
	unsigned int silver_max_freq = 0, gold_max_freq = 0;

	unsigned long load = 100 * util / max;

	if(load < tunables->target_load1){
		freq = (freq + (freq >> tunables->bit_shift1)) * util / max;
	} else if (load >= tunables->target_load1 && load < tunables->target_load2){
		freq = (freq + (freq >> tunables->bit_shift1_2)) * util / max;
	} else {
		freq = (freq - (freq >> tunables->bit_shift2)) * util / max;
	}

	switch(policy->cpu){
	case 0:
		if (state_suspended && silver_max_freq > 0 && silver_max_freq < freq) {
			silver_max_freq = sg_policy->tunables->silver_suspend_max_freq;
			return silver_max_freq;
		}
		break;
	case 1:
	case 2:
	case 3:
		if (state_suspended)
			return policy->min;
		break;
		
	case 4:
		if (state_suspended && gold_max_freq > 0 && gold_max_freq < freq) {
			gold_max_freq = sg_policy->tunables->gold_suspend_max_freq;
			return gold_max_freq; 
		}
		break;
	case 5:
	case 6:
	case 7:
		if (state_suspended)
			return policy->min;
		break;
	default:
		BUG();
	}

	if (freq == sg_policy->cached_raw_freq && sg_policy->next_freq != UINT_MAX)
		return sg_policy->next_freq;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void smugov_get_util(unsigned long *util, unsigned long *max, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long cfs_max;
	struct smugov_cpu *loadcpu = &per_cpu(smugov_cpu, cpu);

	cfs_max = arch_scale_cpu_capacity(NULL, cpu);

	*util = min(rq->cfs.avg.util_avg, cfs_max);
	*max = cfs_max;

	*util = boosted_cpu_util(cpu, &loadcpu->walt_load);
}

static void smugov_set_iowait_boost(struct smugov_cpu *sg_cpu, u64 time,
				   unsigned int flags)
{
	struct smugov_policy *sg_policy = sg_cpu->sg_policy;
	if (!sg_policy->tunables->iowait_boost_enable)
		return;
	if (flags & SCHED_CPUFREQ_IOWAIT) {
		sg_cpu->iowait_boost = sg_cpu->iowait_boost_max;
	} else if (sg_cpu->iowait_boost) {
		s64 delta_ns = time - sg_cpu->last_update;
		/* Clear iowait_boost if the CPU apprears to have been idle. */
		if (delta_ns > TICK_NSEC)
			sg_cpu->iowait_boost = 0;
	}
}

static void smugov_iowait_boost(struct smugov_cpu *sg_cpu, unsigned long *util,
			       unsigned long *max)
{
	unsigned long boost_util = sg_cpu->iowait_boost;
	unsigned long boost_max = sg_cpu->iowait_boost_max;
	if (!boost_util)
		return;
	if (*util * boost_max < *max * boost_util) {
		*util = boost_util;
		*max = boost_max;
	}
	sg_cpu->iowait_boost >>= 1;
}

#ifdef CONFIG_CAPACITY_CLAMPING

static inline
void cap_clamp_cpu_range(unsigned int cpu, unsigned int *cap_min,
			 unsigned int *cap_max)
{
	struct cap_clamp_cpu *cgc;

	*cap_min = 0;
	cgc = &cpu_rq(cpu)->cap_clamp_cpu[CAP_CLAMP_MIN];
	if (cgc->node)
		*cap_min = cgc->value;

	*cap_max = SCHED_CAPACITY_SCALE;
	cgc = &cpu_rq(cpu)->cap_clamp_cpu[CAP_CLAMP_MAX];
	if (cgc->node)
		*cap_max = cgc->value;
}

static inline
unsigned int cap_clamp_cpu_util(unsigned int cpu, unsigned int util)
{
	unsigned int cap_max, cap_min;

	cap_clamp_cpu_range(cpu, &cap_min, &cap_max);
	return clamp(util, cap_min, cap_max);
}

static inline
void cap_clamp_compose(unsigned int *cap_min, unsigned int *cap_max,
		       unsigned int j_cap_min, unsigned int j_cap_max)
{
	*cap_min = max(*cap_min, j_cap_min);
	*cap_max = max(*cap_max, j_cap_max);
}

#define cap_clamp_util_range(util, cap_min, cap_max) \
	clamp_t(typeof(util), util, cap_min, cap_max)

#else

#define cap_clamp_cpu_range(cpu, cap_min, cap_max) { }
#define cap_clamp_cpu_util(cpu, util) util
#define cap_clamp_compose(cap_min, cap_max, j_cap_min, j_cap_max) { }
#define cap_clamp_util_range(util, cap_min, cap_max) util

#endif /* CONFIG_CAPACITY_CLAMPING */

static unsigned long freq_to_util(struct smugov_policy *sg_policy,
				  unsigned int freq)
{
	return mult_frac(sg_policy->max, freq,
			 sg_policy->policy->cpuinfo.max_freq);
}

#define KHZ 1000
static void smugov_track_cycles(struct smugov_policy *sg_policy,
				unsigned int prev_freq,
				u64 upto)
{
	u64 delta_ns, cycles;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	/* Track cycles in current window */
	delta_ns = upto - sg_policy->last_cyc_update_time;
	delta_ns *= prev_freq;
	do_div(delta_ns, (NSEC_PER_SEC / KHZ));
	cycles = delta_ns;
	sg_policy->curr_cycles += cycles;
	sg_policy->last_cyc_update_time = upto;
}

static void smugov_calc_avg_cap(struct smugov_policy *sg_policy, u64 curr_ws,
				unsigned int prev_freq)
{
	u64 last_ws = sg_policy->last_ws;
	unsigned int avg_freq;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	BUG_ON(curr_ws < last_ws);
	if (curr_ws <= last_ws)
		return;

	/* If we skipped some windows */
	if (curr_ws > (last_ws + sched_ravg_window)) {
		avg_freq = prev_freq;
		/* Reset tracking history */
		sg_policy->last_cyc_update_time = curr_ws;
	} else {
		smugov_track_cycles(sg_policy, prev_freq, curr_ws);
		avg_freq = sg_policy->curr_cycles;
		avg_freq /= sched_ravg_window / (NSEC_PER_SEC / KHZ);
	}
	sg_policy->avg_cap = freq_to_util(sg_policy, avg_freq);
	sg_policy->curr_cycles = 0;
	sg_policy->last_ws = curr_ws;
}

#define NL_RATIO 75
#define DEFAULT_HISPEED_LOAD 90
static void smugov_walt_adjust(struct smugov_cpu *sg_cpu, unsigned long *util,
			      unsigned long *max)
{
	struct smugov_policy *sg_policy = sg_cpu->sg_policy;
	bool is_migration = sg_cpu->flags & SCHED_CPUFREQ_INTERCLUSTER_MIG;
	unsigned long nl = sg_cpu->walt_load.nl;
	unsigned long cpu_util = sg_cpu->util;
	bool is_hiload;

	if (unlikely(!sysctl_sched_use_walt_cpu_util))
		return;

	is_hiload = (cpu_util >= mult_frac(sg_policy->avg_cap,
					   sg_policy->tunables->hispeed_load,
					   100));

	if (is_hiload && !is_migration)
		*util = max(*util, sg_policy->hispeed_util);

	if (is_hiload && nl >= mult_frac(cpu_util, NL_RATIO, 100))
		*util = *max;

	if (sg_policy->tunables->pl)
		*util = max(*util, sg_cpu->walt_load.pl);
}

#ifdef CONFIG_NO_HZ_COMMON
static bool smugov_cpu_is_busy(struct smugov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls();
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool smugov_cpu_is_busy(struct smugov_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

static void smugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct smugov_cpu *sg_cpu = container_of(hook, struct smugov_cpu, update_util);
	struct smugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max, hs_util;
	unsigned int next_f;
	bool busy;

	flags &= ~SCHED_CPUFREQ_RT_DL;

	if (!sg_policy->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;

	smugov_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	if (!smugov_should_update_freq(sg_policy, time))
		return;

	busy = smugov_cpu_is_busy(sg_cpu);

	raw_spin_lock(&sg_policy->update_lock);
	if (flags & SCHED_CPUFREQ_RT_DL) {
		/* clear cache when it's bypassed */
		sg_policy->cached_raw_freq = 0;
#ifdef CONFIG_CAPACITY_CLAMPING
		util = cap_clamp_cpu_util(smp_processor_id(),
					  SCHED_CAPACITY_SCALE);
		next_f = get_next_freq(sg_policy, util, policy->cpuinfo.max_freq);
#else
		next_f = policy->cpuinfo.max_freq;
#endif /* CONFIG_CAPACITY_CLAMPING */
	} else {
		smugov_get_util(&util, &max, sg_cpu->cpu);
		if (sg_policy->max != max) {
			sg_policy->max = max;
			hs_util = freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
			hs_util = mult_frac(hs_util, TARGET_LOAD, 100);
			sg_policy->hispeed_util = hs_util;
		}

		sg_cpu->util = util;
		sg_cpu->max = max;
		sg_cpu->flags = flags;
		smugov_calc_avg_cap(sg_policy, sg_cpu->walt_load.ws,
				   sg_policy->policy->cur);
		smugov_iowait_boost(sg_cpu, &util, &max);
		smugov_walt_adjust(sg_cpu, &util, &max);
		util = cap_clamp_cpu_util(smp_processor_id(), util);
		next_f = get_next_freq(sg_policy, util, max);
		/*
		 * Do not reduce the frequency if the CPU has not been idle
		 * recently, as the reduction is likely to be premature then.
		 */
		if (busy && next_f < sg_policy->next_freq) {
			next_f = sg_policy->next_freq;
			/* clear cache when it's bypassed */
			sg_policy->cached_raw_freq = 0;
		}
	}
	smugov_update_commit(sg_policy, time, next_f);
	raw_spin_unlock(&sg_policy->update_lock);
}

static unsigned int smugov_next_freq_shared(struct smugov_cpu *sg_cpu, u64 time)
{
	struct smugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	/* Initialize clamping range based on caller CPU constraints */
	cap_clamp_cpu_range(smp_processor_id(), &cap_min, &cap_max);
	
	for_each_cpu(j, policy->cpus) {
		struct smugov_cpu *j_sg_cpu = &per_cpu(smugov_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		/*
		 * If the CPU utilization was last updated before the previous
		 * frequency update and the time elapsed between the last update
		 * of the CPU utilization and the last frequency update is long
		 * enough, don't take the CPU into account as it probably is
		 * idle now (and clear iowait_boost for it).
		 */
		delta_ns = time - j_sg_cpu->last_update;
		if (delta_ns > stale_ns) {
			j_sg_cpu->iowait_boost = 0;
			continue;
		}
		if (j_sg_cpu->flags & SCHED_CPUFREQ_RT_DL) {
			/* clear cache when it's bypassed */
			sg_policy->cached_raw_freq = 0;
			return policy->cpuinfo.max_freq;
		}
		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (j_util * max >= j_max * util) {
			util = j_util;
			max = j_max;
		}

		smugov_iowait_boost(j_sg_cpu, &util, &max);
		smugov_walt_adjust(j_sg_cpu, &util, &max);

		/*
		 * Update clamping range based on this CPU constraints, but
		 * only if this CPU is not currently idle. Idle CPUs do not
		 * enforce constraints in a shared frequency domain.
		 */
		if (!idle_cpu(j)) {
			cap_clamp_cpu_range(j, &j_cap_min, &j_cap_max);
			cap_clamp_compose(&cap_min, &cap_max,
					  j_cap_min, j_cap_max);
		}
	}
	
	/* Clamp utilization on aggregated CPUs ranges */
	util = cap_clamp_util_range(util, cap_min, cap_max);
	return get_next_freq(sg_policy, util, max);
}

static void smugov_update_shared(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct smugov_cpu *sg_cpu = container_of(hook, struct smugov_cpu, update_util);
	struct smugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max, hs_util;
	unsigned int next_f;

	if (!sg_policy->tunables->pl && flags & SCHED_CPUFREQ_PL)
		return;

	smugov_get_util(&util, &max, sg_cpu->cpu);

	flags &= ~SCHED_CPUFREQ_RT_DL;

	raw_spin_lock(&sg_policy->update_lock);

	if (sg_policy->max != max) {
		sg_policy->max = max;
		hs_util = freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		hs_util = mult_frac(hs_util, TARGET_LOAD, 100);
		sg_policy->hispeed_util = hs_util;
	}

	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->flags = flags;

	smugov_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	smugov_calc_avg_cap(sg_policy, sg_cpu->walt_load.ws,
			   sg_policy->policy->cur);

	if (smugov_should_update_freq(sg_policy, time)) {
		if (flags & SCHED_CPUFREQ_RT_DL) {
			next_f = sg_policy->policy->cpuinfo.max_freq;
			/* clear cache when it's bypassed */
			sg_policy->cached_raw_freq = 0;
		} else {
			next_f = smugov_next_freq_shared(sg_cpu, time);
		}
		smugov_update_commit(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void smugov_work(struct kthread_work *work)
{
	struct smugov_policy *sg_policy = container_of(work, struct smugov_policy, work);
	unsigned long flags;

	mutex_lock(&sg_policy->work_lock);
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	smugov_track_cycles(sg_policy, sg_policy->policy->cur,
			   ktime_get_ns());
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
				CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);

	sg_policy->work_in_progress = false;
}

static void smugov_irq_work(struct irq_work *irq_work)
{
	struct smugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct smugov_policy, irq_work);

	/*
	 * For RT and deadline tasks, the smurfutil governor shoots the
	 * frequency to maximum. Special care must be taken to ensure that this
	 * kthread doesn't result in the same behavior.
	 *
	 * This is (mostly) guaranteed by the work_in_progress flag. The flag is
	 * updated only at the end of the smugov_work() function and before that
	 * the smurfutil governor rejects all other frequency scaling requests.
	 *
	 * There is a very rare case though, where the RT thread yields right
	 * after the work_in_progress flag is cleared. The effects of that are
	 * neglected for now.
	 */
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct smugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct smugov_tunables *to_smugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct smugov_tunables, attr_set);
}

static ssize_t bit_shift1_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->bit_shift1);
}

static ssize_t bit_shift1_2_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->bit_shift1_2);
}

static ssize_t bit_shift2_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->bit_shift2);
}

static ssize_t target_load1_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->target_load1);
}

static ssize_t target_load2_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->target_load2);
}

static ssize_t bit_shift1_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	value = min(max(0,value), 10);
	
	
	if (value == tunables->bit_shift1)
		return count;
		
	tunables->bit_shift1 = value;
	
	return count;
}

static ssize_t bit_shift1_2_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	value = min(max(0,value), 10);
	
	
	if (value == tunables->bit_shift1_2)
		return count;
		
	tunables->bit_shift1_2 = value;
	
	return count;
}

static ssize_t bit_shift2_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	value = min(max(0,value), 10);
	
	
	if (value == tunables->bit_shift2)
		return count;
		
	tunables->bit_shift2 = value;
	
	return count;
}

static ssize_t target_load1_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	value = min(max(0,value), 100);
	
	
	if (value == tunables->target_load1)
		return count;
		
	tunables->target_load1 = value;
	
	return count;
}

static ssize_t target_load2_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	value = min(max(0,value), 100);
	
	
	if (value == tunables->target_load2)
		return count;
		
	tunables->target_load2 = value;
	
	return count;
}

static ssize_t silver_suspend_max_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->silver_suspend_max_freq);
}

static ssize_t silver_suspend_max_freq_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	unsigned int max_freq;

	if (kstrtouint(buf, 10, &max_freq))
		return -EINVAL;

	tunables->silver_suspend_max_freq = max_freq;

	return count;
}

static ssize_t gold_suspend_max_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->gold_suspend_max_freq);
}

static ssize_t gold_suspend_max_freq_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	unsigned int max_freq;

	if (kstrtouint(buf, 10, &max_freq))
		return -EINVAL;

	tunables->gold_suspend_max_freq = max_freq;

	return count;
}

static ssize_t suspend_capacity_factor_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->suspend_capacity_factor);
}

static ssize_t suspend_capacity_factor_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	unsigned int factor;

	if (kstrtouint(buf, 10, &factor))
		return -EINVAL;


	tunables->suspend_capacity_factor = factor;

	return count;
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_us(struct smugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	struct smugov_policy *sg_policy;
	unsigned int rate_limit_us;
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;
	tunables->up_rate_limit_us = rate_limit_us;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}
	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	struct smugov_policy *sg_policy;
	unsigned int rate_limit_us;
	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;
	tunables->down_rate_limit_us = rate_limit_us;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}
	return count;
}

static ssize_t hispeed_load_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_load);
}

static ssize_t hispeed_load_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	if (kstrtouint(buf, 10, &tunables->hispeed_load))
		return -EINVAL;

	tunables->hispeed_load = min(100U, tunables->hispeed_load);

	return count;
}

static ssize_t hispeed_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->hispeed_freq);
}

static ssize_t hispeed_freq_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	unsigned int val;
	struct smugov_policy *sg_policy;
	unsigned long hs_util;
	unsigned long flags;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->hispeed_freq = val;
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		hs_util = freq_to_util(sg_policy,
					sg_policy->tunables->hispeed_freq);
		hs_util = mult_frac(hs_util, TARGET_LOAD, 100);
		sg_policy->hispeed_util = hs_util;
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t pl_show(struct gov_attr_set *attr_set, char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->pl);
}

static ssize_t pl_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);

	if (kstrtobool(buf, &tunables->pl))
		return -EINVAL;

	return count;
}

static ssize_t iowait_boost_enable_show(struct gov_attr_set *attr_set,
					char *buf)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	return sprintf(buf, "%u\n", tunables->iowait_boost_enable);
}

static ssize_t iowait_boost_enable_store(struct gov_attr_set *attr_set,
					 const char *buf, size_t count)
{
	struct smugov_tunables *tunables = to_smugov_tunables(attr_set);
	bool enable;

	if (kstrtobool(buf, &enable))
		return -EINVAL;

	tunables->iowait_boost_enable = enable;

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);
static struct governor_attr hispeed_load = __ATTR_RW(hispeed_load);
static struct governor_attr hispeed_freq = __ATTR_RW(hispeed_freq);
static struct governor_attr pl = __ATTR_RW(pl);
static struct governor_attr iowait_boost_enable = __ATTR_RW(iowait_boost_enable);
static struct governor_attr bit_shift1 = __ATTR_RW(bit_shift1);
static struct governor_attr bit_shift1_2 = __ATTR_RW(bit_shift1_2);
static struct governor_attr bit_shift2 = __ATTR_RW(bit_shift2);
static struct governor_attr target_load1 = __ATTR_RW(target_load1);
static struct governor_attr target_load2 = __ATTR_RW(target_load2);
static struct governor_attr silver_suspend_max_freq = __ATTR_RW(silver_suspend_max_freq);
static struct governor_attr gold_suspend_max_freq = __ATTR_RW(gold_suspend_max_freq);
static struct governor_attr suspend_capacity_factor = __ATTR_RW(suspend_capacity_factor);

static struct attribute *smugov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&hispeed_load.attr,
	&hispeed_freq.attr,
	&pl.attr,
	&iowait_boost_enable.attr,
	&bit_shift1.attr,
	&bit_shift1_2.attr,
	&bit_shift2.attr,
	&target_load1.attr,
	&target_load2.attr,
	&silver_suspend_max_freq.attr,
	&gold_suspend_max_freq.attr,
	&suspend_capacity_factor.attr,
	NULL
};

static struct kobj_type smugov_tunables_ktype = {
	.default_attrs = smugov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor smurfutil_gov;

static struct smugov_policy *smugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void smugov_policy_free(struct smugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int smugov_kthread_create(struct smugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, smugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"smugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create smugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, smugov_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void smugov_kthread_stop(struct smugov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct smugov_tunables *smugov_tunables_alloc(struct smugov_policy *sg_policy)
{
	struct smugov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void smugov_tunables_save(struct cpufreq_policy *policy,
		struct smugov_tunables *tunables)
{
	int cpu;
	struct smugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached) {
			pr_warn("Couldn't allocate tunables for caching\n");
			return;
		}
		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	cached->pl = tunables->pl;
	cached->hispeed_load = tunables->hispeed_load;
	cached->hispeed_freq = tunables->hispeed_freq;
	cached->up_rate_limit_us = tunables->up_rate_limit_us;
	cached->down_rate_limit_us = tunables->down_rate_limit_us;
	cached->bit_shift1 = tunables->bit_shift1;
	cached->bit_shift1_2 = tunables->bit_shift1_2;
	cached->bit_shift2 = tunables->bit_shift2;
	cached->target_load1 = tunables->target_load1;
	cached->target_load2 = tunables->target_load2;
	cached->silver_suspend_max_freq = tunables->silver_suspend_max_freq;
	cached->gold_suspend_max_freq = tunables->gold_suspend_max_freq;	
	cached->suspend_capacity_factor = tunables->suspend_capacity_factor;
}

static void smugov_tunables_free(struct smugov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

static void smugov_tunables_restore(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy = policy->governor_data;
	struct smugov_tunables *tunables = sg_policy->tunables;
	struct smugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached)
		return;

	tunables->pl = cached->pl;
	tunables->hispeed_load = cached->hispeed_load;
	tunables->hispeed_freq = cached->hispeed_freq;
	tunables->up_rate_limit_us = cached->up_rate_limit_us;
	sg_policy->up_rate_delay_ns = cached->up_rate_limit_us;
	tunables->bit_shift1 = cached->bit_shift1;
	tunables->bit_shift1_2 = cached->bit_shift1_2;
	tunables->bit_shift2 = cached->bit_shift2;
	tunables->target_load1 = cached->target_load1;
	tunables->target_load2 = cached->target_load2;
	tunables->silver_suspend_max_freq = cached->silver_suspend_max_freq;
	tunables->gold_suspend_max_freq = cached->gold_suspend_max_freq;	
	tunables->suspend_capacity_factor = cached->suspend_capacity_factor;
	sg_policy->down_rate_delay_ns = cached->down_rate_limit_us;
	update_min_rate_limit_us(sg_policy);
}

static int smugov_init(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy;
	struct smugov_tunables *tunables;
	unsigned int cpu = cpumask_first(policy->related_cpus);
	unsigned int lat;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = smugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = smugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = smugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->pl = 1;
	tunables->up_rate_limit_us = LATENCY_MULTIPLIER;
	tunables->down_rate_limit_us = LATENCY_MULTIPLIER;
	tunables->hispeed_load = DEFAULT_HISPEED_LOAD;
	tunables->hispeed_freq = 0;
	lat = policy->cpuinfo.transition_latency / NSEC_PER_USEC;
	if (lat) {
		tunables->up_rate_limit_us *= lat;
		tunables->down_rate_limit_us *= lat;
	}
	tunables->silver_suspend_max_freq = DEFAULT_SUSPEND_MAX_FREQ_SILVER;
	tunables->gold_suspend_max_freq = DEFAULT_SUSPEND_MAX_FREQ_GOLD;
	tunables->suspend_capacity_factor = DEFAULT_SUSPEND_CAPACITY_FACTOR;

	if (cpu < 4){
		tunables->up_rate_limit_us = LATENCY_MULTIPLIER;
		tunables->down_rate_limit_us = LATENCY_MULTIPLIER;
		tunables->bit_shift1 = BIT_SHIFT_1;
		tunables->bit_shift1_2 = BIT_SHIFT_1_2;
		tunables->bit_shift2 = BIT_SHIFT_2;
		tunables->target_load1 = TARGET_LOAD_1;
		tunables->target_load2 = TARGET_LOAD_2;
	} else {
		tunables->up_rate_limit_us = LATENCY_MULTIPLIER;
		tunables->down_rate_limit_us = LATENCY_MULTIPLIER;
		tunables->bit_shift1 = BIT_SHIFT_1_BIGC;
		tunables->bit_shift1_2 = BIT_SHIFT_1_2_BIGC;
		tunables->bit_shift2 = BIT_SHIFT_2_BIGC;
		tunables->target_load1 = TARGET_LOAD_1_BIGC;
		tunables->target_load2 = TARGET_LOAD_2_BIGC;
	}

	tunables->iowait_boost_enable = false;

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;
	stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	smugov_tunables_restore(policy);

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &smugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   smurfutil_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	policy->governor_data = NULL;
	smugov_tunables_free(tunables);

stop_kthread:
	smugov_kthread_stop(sg_policy);

free_sg_policy:
	mutex_unlock(&global_tunables_lock);

	smugov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void smugov_exit(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy = policy->governor_data;
	struct smugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		smugov_tunables_save(policy, tunables);
		smugov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	smugov_kthread_stop(sg_policy);
	smugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int smugov_start(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_us(sg_policy);
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = UINT_MAX;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct smugov_cpu *sg_cpu = &per_cpu(smugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->sg_policy = sg_policy;
		sg_cpu->cpu = cpu;
		sg_cpu->flags = SCHED_CPUFREQ_RT;
		sg_cpu->iowait_boost_max = policy->cpuinfo.max_freq;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct smugov_cpu *sg_cpu = &per_cpu(smugov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							smugov_update_shared :
							smugov_update_single);
	}
	return 0;
}

static void smugov_stop(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_sched();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void smugov_limits(struct cpufreq_policy *policy)
{
	struct smugov_policy *sg_policy = policy->governor_data;
	unsigned long flags;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		smugov_track_cycles(sg_policy, sg_policy->policy->cur,
				   ktime_get_ns());
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	sg_policy->need_freq_update = true;
}

static struct cpufreq_governor smurfutil_gov = {
	.name = "smurfutil",
	.owner = THIS_MODULE,
	.init = smugov_init,
	.exit = smugov_exit,
	.start = smugov_start,
	.stop = smugov_stop,
	.limits = smugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_PIXEL_SMURFUTIL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &smurfutil_gov;
}
#endif

static int __init smugov_register(void)
{
	return cpufreq_register_governor(&smurfutil_gov);
}
fs_initcall(smugov_register);
