// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/sort.h>
#include <linux/version.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

/* SEND_SIG_FORCED isn't present in newer kernels */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#define SIG_INFO_TYPE SEND_SIG_FORCED
#else
#define SIG_INFO_TYPE SEND_SIG_PRIV
#endif

/* The group argument to do_send_sig_info is different in newer kernels */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
#define KILL_GROUP_TYPE true
#else
#define KILL_GROUP_TYPE PIDTYPE_TGID
#endif

/* The minimum number of pages to free per reclaim */
#define MIN_FREE_PAGES (CONFIG_ANDROID_SIMPLE_LMK_MINFREE * SZ_1M / PAGE_SIZE)

/* Kill up to this many victims per reclaim */
#define MAX_VICTIMS 1024

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;
};

/* Pulled from the Android framework. Lower ADJ means higher priority. */
static const short int adj_prio[] = {
	906, /* CACHED_APP_MAX_ADJ */
	905, /* Cached app */
	904, /* Cached app */
	903, /* Cached app */
	902, /* Cached app */
	901, /* Cached app */
	900, /* CACHED_APP_MIN_ADJ */
	800, /* SERVICE_B_ADJ */
	700, /* PREVIOUS_APP_ADJ */
	600, /* HOME_APP_ADJ */
	500, /* SERVICE_ADJ */
	400, /* HEAVY_WEIGHT_APP_ADJ */
	300, /* BACKUP_APP_ADJ */
	200, /* PERCEPTIBLE_APP_ADJ */
	100, /* VISIBLE_APP_ADJ */
	0    /* FOREGROUND_APP_ADJ */
};

static struct victim_info victims[MAX_VICTIMS];
static DECLARE_WAIT_QUEUE_HEAD(oom_waitq);
static DECLARE_COMPLETION(reclaim_done);
static int victims_to_kill;
static bool needs_reclaim;

static int victim_info_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	return rhs->size - lhs->size;
}

static bool mm_is_duplicate(struct victim_info *varr, int vlen,
			    struct mm_struct *mm)
{
	int i;

	for (i = 0; i < vlen; i++) {
		if (varr[i].mm == mm)
			return true;
	}

	return false;
}

static bool vtsk_is_duplicate(struct victim_info *varr, int vlen,
			      struct task_struct *vtsk)
{
	int i;

	for (i = 0; i < vlen; i++) {
		if (same_thread_group(varr[i].tsk, vtsk))
			return true;
	}

	return false;
}

static unsigned long find_victims(struct victim_info *varr, int *vindex,
				  int vmaxlen, int min_adj, int max_adj)
{
	unsigned long pages_found = 0;
	int old_vindex = *vindex;
	struct task_struct *tsk;

	for_each_process(tsk) {
		struct task_struct *vtsk;
		unsigned long tasksize;
		short oom_score_adj;

		/* Make sure there's space left in the victim array */
		if (*vindex == vmaxlen)
			break;

		/* Don't kill kthreads, init, or duplicates */
		if (tsk->flags & PF_KTHREAD || is_global_init(tsk) ||
		    vtsk_is_duplicate(varr, *vindex, tsk))
			continue;

		vtsk = find_lock_task_mm(tsk);
		if (!vtsk)
			continue;

		/* Skip tasks that lack memory or have a redundant mm */
		if (test_tsk_thread_flag(vtsk, TIF_MEMDIE) ||
		    mm_is_duplicate(varr, *vindex, vtsk->mm))
			goto unlock_mm;

		/* Check the task's importance (adj) to see if it's in range */
		oom_score_adj = vtsk->signal->oom_score_adj;
		if (oom_score_adj < min_adj || oom_score_adj > max_adj)
			goto unlock_mm;

		/* Get the total number of physical pages in use by the task */
		tasksize = get_mm_rss(vtsk->mm);
		if (!tasksize)
			goto unlock_mm;

		/* Store this potential victim away for later */
		varr[*vindex].tsk = vtsk;
		varr[*vindex].mm = vtsk->mm;
		varr[*vindex].size = tasksize;
		(*vindex)++;

		/* Keep track of the number of pages that have been found */
		pages_found += tasksize;
		continue;

unlock_mm:
		task_unlock(vtsk);
	}

	/*
	 * Sort the victims in descending order of size to prioritize killing
	 * the larger ones first.
	 */
	if (pages_found)
		sort(&varr[old_vindex], *vindex - old_vindex, sizeof(*varr),
		     victim_info_cmp, NULL);

	return pages_found;
}

static void scan_and_kill(unsigned long pages_needed)
{
	int i, nr_to_kill = 0, nr_victims = 0;
	unsigned long pages_found = 0;

	/*
	 * Hold the tasklist lock so tasks don't disappear while scanning. This
	 * is preferred to holding an RCU read lock so that the list of tasks
	 * is guaranteed to be up to date.
	 */
	read_lock(&tasklist_lock);
	for (i = 1; i < ARRAY_SIZE(adj_prio); i++) {
		pages_found += find_victims(victims, &nr_victims, MAX_VICTIMS,
					    adj_prio[i], adj_prio[i - 1]);
		if (pages_found >= pages_needed || nr_victims == MAX_VICTIMS)
			break;
	}

	/*
	 * Calculate the number of tasks that need to be killed and quickly
	 * release the references to those that'll live.
	 */
	for (i = 0, pages_found = 0; i < nr_victims; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		/* The victim's mm lock is taken in find_victims; release it */
		if (pages_found >= pages_needed) {
			task_unlock(vtsk);
			continue;
		}

		/*
		 * Grab a reference to the victim so it doesn't disappear after
		 * its mm lock is released later.
		 */
		get_task_struct(vtsk);
		pages_found += victim->size;
		nr_to_kill++;
	}
	read_unlock(&tasklist_lock);

	/* Kill the victims */
	WRITE_ONCE(victims_to_kill, nr_to_kill);
	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		pr_info("Killing %s with adj %d to free %lu kiB\n", vtsk->comm,
			vtsk->signal->oom_score_adj,
			victim->size << (PAGE_SHIFT - 10));

		/* Accelerate the victim's death by forcing the kill signal */
		do_send_sig_info(SIGKILL, SIG_INFO_TYPE, vtsk, KILL_GROUP_TYPE);

		/* Finally release the victim's mm lock */
		task_unlock(vtsk);
	}

	/* Try to speed up the death process now that we can schedule again */
	for (i = 0; i < nr_to_kill; i++) {
		struct task_struct *vtsk = victims[i].tsk;

		/* Increase the victim's priority to make it die faster */
		set_user_nice(vtsk, MIN_NICE);

		/* Allow the victim to run on any CPU */
		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		/* Finally release the victim reference acquired earlier */
		put_task_struct(vtsk);
	}

	/* Wait until all the victims die */
	wait_for_completion(&reclaim_done);
}

static int simple_lmk_reclaim_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop;

		wait_event(oom_waitq, (should_stop = kthread_should_stop()) ||
				      READ_ONCE(needs_reclaim));

		if (should_stop)
			break;

		/*
		 * Kill a batch of processes and wait for their memory to be
		 * freed. After their memory is freed, sleep for 20 ms to give
		 * OOM'd allocations a chance to scavenge for the newly-freed
		 * pages. Rinse and repeat while there are still OOM'd
		 * allocations.
		 */
		do {
			scan_and_kill(MIN_FREE_PAGES);
			msleep(20);
		} while (READ_ONCE(needs_reclaim));
	}

	return 0;
}

void simple_lmk_decide_reclaim(int kswapd_priority)
{
	if (kswapd_priority != CONFIG_ANDROID_SIMPLE_LMK_AGGRESSION)
		return;

	if (!cmpxchg(&needs_reclaim, false, true))
		wake_up(&oom_waitq);
}

void simple_lmk_stop_reclaim(void)
{
	WRITE_ONCE(needs_reclaim, false);
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	static atomic_t nr_killed = ATOMIC_INIT(0);
	int i, nr_to_kill;

	nr_to_kill = READ_ONCE(victims_to_kill);
	for (i = 0; i < nr_to_kill; i++) {
		if (victims[i].mm == mm) {
			if (atomic_inc_return(&nr_killed) == nr_to_kill) {
				WRITE_ONCE(victims_to_kill, 0);
				nr_killed = (atomic_t)ATOMIC_INIT(0);
				complete(&reclaim_done);
			}
			break;
		}
	}
}

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static bool init_done;
	struct task_struct *thread;

	if (cmpxchg(&init_done, false, true))
		return 0;

	thread = kthread_run_perf_critical(simple_lmk_reclaim_thread, NULL,
					   "simple_lmkd");
	BUG_ON(IS_ERR(thread));

	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);
