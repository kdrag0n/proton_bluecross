/*
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "devfreq_boost: " fmt

#include <linux/devfreq_boost.h>
#include <linux/fb.h>

struct df_boost_drv {
	struct boost_dev devices[DEVFREQ_MAX];
};

static struct df_boost_drv *df_boost_drv_g __read_mostly;

static void __devfreq_boost_kick_max(struct boost_dev *b,
	unsigned int duration_ms)
{
	unsigned long flags, new_expires;

	spin_lock_irqsave(&b->lock, flags);
	if (!b->df) {
		spin_unlock_irqrestore(&b->lock, flags);
		return;
	}

	new_expires = jiffies + b->max_boost_jiffies;
	if (time_after(b->max_boost_expires, new_expires)) {
		spin_unlock_irqrestore(&b->lock, flags);
		return;
	}
	b->max_boost_expires = new_expires;
	b->max_boost_jiffies = msecs_to_jiffies(duration_ms);
	spin_unlock_irqrestore(&b->lock, flags);

	queue_work(b->wq, &b->max_boost);
}

void devfreq_boost_kick_max(enum df_device device, unsigned int duration_ms)
{
	struct df_boost_drv *d = df_boost_drv_g;

	if (!d)
		return;

	__devfreq_boost_kick_max(d->devices + device, duration_ms);
}

void devfreq_register_boost_device(enum df_device device, struct devfreq *df)
{
	struct df_boost_drv *d = df_boost_drv_g;
	struct boost_dev *b;
	unsigned long flags;

	if (!d)
		return;

	df->is_boost_device = true;

	b = d->devices + device;
	spin_lock_irqsave(&b->lock, flags);
	b->df = df;
	spin_unlock_irqrestore(&b->lock, flags);
}

struct boost_dev *devfreq_get_boost_dev(enum df_device device)
{
	struct df_boost_drv *d = df_boost_drv_g;

	if (!d)
		return NULL;

	return d->devices + device;
}

static void devfreq_max_boost(struct work_struct *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), max_boost);
	unsigned long boost_jiffies, flags;

	if (!cancel_delayed_work_sync(&b->max_unboost)) {
		struct devfreq *df = b->df;

		mutex_lock(&df->lock);
		df->max_boost = true;
		update_devfreq(df);
		mutex_unlock(&df->lock);
	}

	spin_lock_irqsave(&b->lock, flags);
	boost_jiffies = b->max_boost_jiffies;
	spin_unlock_irqrestore(&b->lock, flags);

	queue_delayed_work(b->wq, &b->max_unboost, boost_jiffies);
}

static void devfreq_max_unboost(struct work_struct *work)
{
	struct boost_dev *b =
		container_of(to_delayed_work(work), typeof(*b), max_unboost);
	struct devfreq *df = b->df;

	mutex_lock(&df->lock);
	df->max_boost = false;
	update_devfreq(df);
	mutex_unlock(&df->lock);
}

static int __init devfreq_boost_init(void)
{
	struct df_boost_drv *d;
	struct workqueue_struct *wq;
	int i, ret;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	wq = alloc_workqueue("devfreq_boost_wq", WQ_HIGHPRI, 0);
	if (!wq) {
		ret = -ENOMEM;
		goto free_d;
	}

	for (i = 0; i < DEVFREQ_MAX; i++) {
		struct boost_dev *b = d->devices + i;

		b->wq = wq;
		b->abs_min_freq = ULONG_MAX;
		spin_lock_init(&b->lock);
		INIT_WORK(&b->max_boost, devfreq_max_boost);
		INIT_DELAYED_WORK(&b->max_unboost, devfreq_max_unboost);
	}

	df_boost_drv_g = d;

	return 0;

free_d:
	kfree(d);
	return ret;
}
subsys_initcall(devfreq_boost_init);
