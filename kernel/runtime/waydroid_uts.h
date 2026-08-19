/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KSU_WAYDROID_UTS_H
#define __KSU_WAYDROID_UTS_H

#include <linux/sched.h>

#ifdef CONFIG_KSU_NON_ANDROID
int ksu_waydroid_uts_apply_once(struct task_struct *task);
void ksu_waydroid_uts_restore(void);
#else
static inline int ksu_waydroid_uts_apply_once(struct task_struct *task)
{
	return 0;
}

static inline void ksu_waydroid_uts_restore(void)
{
}
#endif

#endif /* __KSU_WAYDROID_UTS_H */
