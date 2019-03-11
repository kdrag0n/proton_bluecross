/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2019 Danny Lin <danny@kdrag0n.dev>.
 */

#ifndef _COREPOWER_H_
#define _COREPOWER_H_

#ifdef CONFIG_COREPOWER
void corepower_wake(void);
#else
static inline void corepower_wake(void)
{
}
#endif

#endif /* _COREPOWER_H_ */
