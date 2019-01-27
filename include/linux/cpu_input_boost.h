// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_INPUT_BOOST_H_
#define _CPU_INPUT_BOOST_H_

#ifdef CONFIG_CPU_INPUT_BOOST
extern unsigned long last_input_jiffies;
extern int vidc_active_instances;

void cpu_input_boost_kick(void);
void cpu_input_boost_kick_max(unsigned int duration_ms);
void cpu_input_boost_kick_general(unsigned int duration_ms);

static inline bool should_kick_frame_boost(void)
{
	unsigned int timeout = 2500;

	if (vidc_active_instances > 0)
		timeout = 500;

	return time_before(jiffies,
			   last_input_jiffies + msecs_to_jiffies(timeout));
}
#else
static inline void cpu_input_boost_kick(void)
{
}
static inline void cpu_input_boost_kick_max(unsigned int duration_ms)
{
}
static inline void cpu_input_boost_kick_general(unsigned int duration_ms)
{
}

static inline bool should_kick_frame_boost(void)
{
	return false;
}
#endif

#endif /* _CPU_INPUT_BOOST_H_ */
