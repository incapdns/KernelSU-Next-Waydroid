// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cred.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/susfs.h>
#include <linux/susfs_def.h>
#include <linux/susfs_ksu.h>

#include "feature/susfs_bridge.h"
#include "feature/selinux_hide.h"
#include "ksu.h"
#include "selinux/selinux.h"

#ifdef CONFIG_KSU_SELINUX
#include "selinux/sepolicy.h"
extern struct selinux_policy *backup_sepolicy;
#endif

static bool ksu_susfs_is_current_domain(void)
{
#ifdef CONFIG_KSU_SELINUX
	if (is_ksu_domain())
		return true;
#endif
	return current_uid().val == 0 &&
	       task_active_pid_ns(current) != &init_pid_ns;
}

static struct selinux_policy *ksu_susfs_get_backup_sepolicy(void)
{
#ifdef CONFIG_KSU_SELINUX
	return READ_ONCE(backup_sepolicy);
#else
	return NULL;
#endif
}

static const struct susfs_ksu_ops ksu_susfs_ops = {
	.is_current_ksu_domain = ksu_susfs_is_current_domain,
	.get_backup_sepolicy = ksu_susfs_get_backup_sepolicy,
};

int ksu_susfs_bridge_init(void)
{
	int ret = susfs_ksu_register(ksu_cred, &ksu_susfs_ops);

	if (ret)
		pr_err("susfs: failed to register KernelSU bridge: %d\n", ret);
	else
		ksu_susfs_bridge_sync_selinux();
	return ret;
}

void ksu_susfs_bridge_exit(void)
{
	susfs_ksu_unregister(&ksu_susfs_ops);
}

void ksu_susfs_bridge_post_fs_data(void)
{
	susfs_start_sdcard_monitor_fn();
	ksu_susfs_bridge_sync_selinux();
}

void ksu_susfs_bridge_mark_app(uid_t old_uid, uid_t new_uid)
{
	if (old_uid != 0 || new_uid < 10000)
		return;
#ifdef CONFIG_KSU_NON_ANDROID
	/*
	 * Waydroid's Zygote runs under the host SELinux policy and therefore
	 * legitimately retains initrc_t instead of Android's zygote SID.  The
	 * syscall dispatcher has already restricted this path to the registered
	 * Waydroid PID namespace, so use the namespace plus the canonical
	 * Zygote UID transition here.  Host tasks never reach this branch.
	 */
	if (task_active_pid_ns(current) == &init_pid_ns)
		return;
#elif defined(CONFIG_KSU_SELINUX)
	if (!is_zygote(current_cred()))
		return;
#else
	if (task_active_pid_ns(current) == &init_pid_ns)
		return;
#endif
	susfs_set_current_proc_umounted();
	susfs_schedule_extra_work();
}

void ksu_susfs_bridge_sync_selinux(void)
{
	susfs_ksu_set_selinux_state(ksu_selinux_hide_is_enabled(),
				      ksu_selinux_hide_is_running(),
				      ksu_get_cached_su_sid(),
				      ksu_get_cached_zygote_sid(),
				      ksu_get_cached_priv_app_sid());
}

long ksu_susfs_bridge_dispatch(unsigned int cmd, unsigned long user_arg)
{
	void __user *arg = (void __user *)user_arg;

	return susfs_ksu_dispatch(cmd, &arg);
}
