// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_INPUT_BOOST_H_
#define _CPU_INPUT_BOOST_H_

#ifdef CONFIG_CPU_INPUT_BOOST
extern unsigned long last_input_jiffies;

void cpu_input_boost_kick(void);
void cpu_input_boost_kick_max(unsigned int duration_ms);
void cpu_input_boost_kick_general(unsigned int duration_ms);

static inline bool should_kick_frame_boost(void)
{
	return time_before(jiffies,
			   last_input_jiffies + msecs_to_jiffies(3250));
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
