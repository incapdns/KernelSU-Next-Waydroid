// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/rcupdate.h>
#include <linux/statfs.h>
#include <linux/wait.h>

#include "feature/overlayfs_statfs_hide.h"
#include "hook/patch_memory.h"
#include "hook/syscall_hook_manager.h"
#include "infra/symbol_resolver.h"
#include "policy/allowlist.h"

typedef int (*overlay_statfs_fn)(struct dentry *dentry,
				 struct kstatfs *buf);
typedef void (*overlay_path_lower_fn)(struct dentry *dentry,
				      struct path *path);

enum overlay_statfs_hook_state {
	OVERLAY_STATFS_HOOK_INACTIVE,
	OVERLAY_STATFS_HOOK_ACTIVE,
	OVERLAY_STATFS_HOOK_DRAINING,
	OVERLAY_STATFS_HOOK_DETACHED,
	OVERLAY_STATFS_HOOK_FAILED,
};

static DEFINE_MUTEX(overlay_statfs_hook_lock);
static DECLARE_WAIT_QUEUE_HEAD(overlay_statfs_callback_waitq);
static atomic_t overlay_statfs_active_callbacks = ATOMIC_INIT(0);
static atomic_t overlay_statfs_state =
	ATOMIC_INIT(OVERLAY_STATFS_HOOK_INACTIVE);
static struct super_operations *overlay_super_ops;
static overlay_statfs_fn original_overlay_statfs;
static overlay_path_lower_fn overlay_path_lower;
static struct module *overlay_owner;
static bool overlay_statfs_hook_installed;
static bool overlay_statfs_filter_enabled;

static const char *overlay_statfs_state_name(int state)
{
	switch (state) {
	case OVERLAY_STATFS_HOOK_INACTIVE:
		return "inactive";
	case OVERLAY_STATFS_HOOK_ACTIVE:
		return "active";
	case OVERLAY_STATFS_HOOK_DRAINING:
		return "draining";
	case OVERLAY_STATFS_HOOK_DETACHED:
		return "detached";
	case OVERLAY_STATFS_HOOK_FAILED:
		return "failed";
	default:
		return "invalid";
	}
}

static int ksu_overlay_statfs_state_get(char *buffer,
					const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s callbacks=%d installed=%c\n",
			  overlay_statfs_state_name(
				  atomic_read(&overlay_statfs_state)),
			  atomic_read(&overlay_statfs_active_callbacks),
			  READ_ONCE(overlay_statfs_hook_installed) ? 'Y' : 'N');
}

static const struct kernel_param_ops ksu_overlay_statfs_state_ops = {
	.get = ksu_overlay_statfs_state_get,
};

module_param_cb(overlay_statfs_hook_state,
		&ksu_overlay_statfs_state_ops, NULL, 0400);
MODULE_PARM_DESC(overlay_statfs_hook_state,
		 "Dynamic OverlayFS statfs hook lifecycle and callback count");

static bool ksu_should_hide_overlay_statfs(void)
{
	uid_t uid;

	if (!READ_ONCE(overlay_statfs_filter_enabled) ||
	    !ksu_is_waydroid_task(current))
		return false;

	uid = current_euid().val;
	return uid != 0 && !ksu_is_allow_uid_for_current(uid);
}

static int ksu_overlay_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	overlay_statfs_fn original;
	overlay_path_lower_fn path_lower;
	struct path lower = { };
	int ret;

	/* The raw super_operations pointer cannot pin this module.  Pair the
	 * permanent unload guard with a per-call reference and callback counter,
	 * exactly as for the dynamic /proc/modules hook. */
	if (unlikely(!try_module_get(THIS_MODULE))) {
		original = READ_ONCE(original_overlay_statfs);
		return original ? original(dentry, buf) : -ENODEV;
	}

	atomic_inc(&overlay_statfs_active_callbacks);
	smp_mb__after_atomic();
	original = READ_ONCE(original_overlay_statfs);
	path_lower = READ_ONCE(overlay_path_lower);

	if (unlikely(!original)) {
		ret = -ENODEV;
	} else if (!ksu_should_hide_overlay_statfs() || !path_lower) {
		ret = original(dentry, buf);
	} else {
		/* Android read-only partitions are lower OverlayFS paths.  Querying
		 * the real lower mount preserves its complete kstatfs (type, fsid,
		 * sizes, flags) for both statfs() and fstatfs(), instead of merely
		 * replacing OVERLAYFS_SUPER_MAGIC in the userspace result. */
		path_lower(dentry, &lower);
		if (lower.mnt && lower.dentry)
			ret = vfs_statfs(&lower, buf);
		else
			ret = original(dentry, buf);
	}

	if (atomic_dec_and_test(&overlay_statfs_active_callbacks))
		wake_up_all(&overlay_statfs_callback_waitq);
	module_put(THIS_MODULE);
	return ret;
}

int ksu_overlayfs_statfs_hide_init(void)
{
	overlay_statfs_fn replacement = ksu_overlay_statfs;
	unsigned long ops_address;
	unsigned long lower_address;
	int ret = 0;

	if (!THIS_MODULE)
		return 0;

	mutex_lock(&overlay_statfs_hook_lock);
	if (overlay_statfs_hook_installed) {
		ret = -EALREADY;
		goto out;
	}

	/* kallsyms_on_each_match_symbol() covers vmlinux only. OverlayFS is a
	 * module on CachyOS, so query its private kallsyms explicitly and retain
	 * its owner while a pointer inside its read-only data is patched. */
	ops_address = module_kallsyms_lookup_name(
		"overlay:ovl_super_operations");
	lower_address = module_kallsyms_lookup_name("overlay:ovl_path_lower");
	if (ops_address && lower_address) {
		overlay_owner = find_module("overlay");
		if (!overlay_owner || !try_module_get(overlay_owner)) {
			ret = -ENODEV;
			goto failed;
		}
	} else {
		ops_address = find_kernel_symbol_exact("ovl_super_operations");
		lower_address = (unsigned long)
			ksu_resolve_symbol_for_functable_hook("ovl_path_lower");
	}
	overlay_super_ops = (struct super_operations *)ops_address;
	overlay_path_lower = (overlay_path_lower_fn)lower_address;
	if (!overlay_super_ops || !READ_ONCE(overlay_super_ops->statfs) ||
	    !overlay_path_lower) {
		pr_warn("overlayfs_statfs_hide: symbols unavailable (ops=%px lower=%px)\n",
			overlay_super_ops, overlay_path_lower);
		ret = -ENOENT;
		goto failed;
	}

	original_overlay_statfs = READ_ONCE(overlay_super_ops->statfs);
	if (original_overlay_statfs == replacement) {
		ret = -EALREADY;
		goto failed;
	}

	WRITE_ONCE(overlay_statfs_filter_enabled, true);
	smp_wmb();
	ret = ksu_patch_text(&overlay_super_ops->statfs, &replacement,
			     sizeof(replacement), KSU_PATCH_TEXT_FLUSH_DCACHE);
	if (ret || READ_ONCE(overlay_super_ops->statfs) != replacement) {
		pr_err("overlayfs_statfs_hide: cannot patch super_operations: %d\n",
		       ret);
		if (!ret)
			ret = -EIO;
		WRITE_ONCE(overlay_statfs_filter_enabled, false);
		goto failed;
	}

	WRITE_ONCE(overlay_statfs_hook_installed, true);
	atomic_set(&overlay_statfs_state, OVERLAY_STATFS_HOOK_ACTIVE);
	pr_info("overlayfs_statfs_hide: statfs patched at %px (original %px)\n",
		&overlay_super_ops->statfs, original_overlay_statfs);
	goto out;

failed:
	if (overlay_owner) {
		module_put(overlay_owner);
		overlay_owner = NULL;
	}
	atomic_set(&overlay_statfs_state, OVERLAY_STATFS_HOOK_FAILED);
out:
	mutex_unlock(&overlay_statfs_hook_lock);
	return ret;
}

int ksu_overlayfs_statfs_hide_prepare_unload(void)
{
	overlay_statfs_fn original;
	long drained;
	int ret = 0;

	if (!THIS_MODULE)
		return 0;

	mutex_lock(&overlay_statfs_hook_lock);
	if (atomic_read(&overlay_statfs_state) == OVERLAY_STATFS_HOOK_DETACHED)
		goto out;

	atomic_set(&overlay_statfs_state, OVERLAY_STATFS_HOOK_DRAINING);
	WRITE_ONCE(overlay_statfs_filter_enabled, false);
	smp_mb();

	if (overlay_statfs_hook_installed) {
		original = READ_ONCE(original_overlay_statfs);
		if (!overlay_super_ops || !original) {
			ret = -EINVAL;
			goto failed;
		}
		ret = ksu_patch_text(&overlay_super_ops->statfs, &original,
				     sizeof(original),
				     KSU_PATCH_TEXT_FLUSH_DCACHE);
		if (ret || READ_ONCE(overlay_super_ops->statfs) != original) {
			pr_emerg("overlayfs_statfs_hide: failed to restore callback: %d\n",
				 ret);
			if (!ret)
				ret = -EIO;
			goto failed;
		}
		WRITE_ONCE(overlay_statfs_hook_installed, false);
		pr_emerg("overlayfs_statfs_hide: callback restored\n");
	}

	synchronize_rcu_tasks();
	drained = wait_event_timeout(overlay_statfs_callback_waitq,
			atomic_read(&overlay_statfs_active_callbacks) == 0,
			msecs_to_jiffies(2000));
	if (!drained && atomic_read(&overlay_statfs_active_callbacks) != 0) {
		pr_emerg("overlayfs_statfs_hide: %d callbacks did not drain\n",
			 atomic_read(&overlay_statfs_active_callbacks));
		ret = -EBUSY;
		goto failed;
	}

	synchronize_rcu();
	msleep(20);
	if (atomic_read(&overlay_statfs_active_callbacks) != 0) {
		ret = -EBUSY;
		goto failed;
	}

	WRITE_ONCE(overlay_path_lower, NULL);
	if (overlay_owner) {
		module_put(overlay_owner);
		overlay_owner = NULL;
	}
	atomic_set(&overlay_statfs_state, OVERLAY_STATFS_HOOK_DETACHED);
	pr_emerg("overlayfs_statfs_hide: callback tasks and references drained\n");
	goto out;

failed:
	atomic_set(&overlay_statfs_state, OVERLAY_STATFS_HOOK_DRAINING);
out:
	mutex_unlock(&overlay_statfs_hook_lock);
	return ret;
}

void ksu_overlayfs_statfs_hide_exit(void)
{
	int ret = ksu_overlayfs_statfs_hide_prepare_unload();

	if (ret)
		pr_emerg("overlayfs_statfs_hide: unsafe exit prevented earlier: %d\n",
			 ret);
}
