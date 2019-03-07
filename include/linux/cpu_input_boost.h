// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_INPUT_BOOST_H_
#define _CPU_INPUT_BOOST_H_

#include <linux/types.h>

#ifdef CONFIG_CPU_INPUT_BOOST
extern unsigned long last_input_jiffies;

void cpu_input_boost_kick(void);
void cpu_input_boost_kick_max(unsigned int duration_ms);

bool cpu_input_boost_should_boost_frame(void);
#else
static inline void cpu_input_boost_kick(void)
{
}
static inline void cpu_input_boost_kick_max(unsigned int duration_ms)
{
}

static inline bool cpu_input_boost_should_boost_frame(void)
{
	return false;
}
#endif

#endif /* _CPU_INPUT_BOOST_H_ */
