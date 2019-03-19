/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 Danny Lin <danny@kdrag0n.dev>.
 */

#ifndef _DISPLAY_STATE_H_
#define _DISPLAY_STATE_H_

#ifdef CONFIG_DRM_MSM
bool is_display_on(void);
#else
static inline bool is_display_on(void)
{
	return true;
}
#endif

#endif /* _DISPLAY_STATE_H_ */
