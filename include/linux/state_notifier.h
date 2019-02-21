#ifndef __LINUX_STATE_NOTIFIER_H
#define __LINUX_STATE_NOTIFIER_H

#include <linux/notifier.h>

extern bool state_suspended;
extern void state_suspend(void);
extern void state_resume(void);

#endif /* _LINUX_STATE_NOTIFIER_H */
