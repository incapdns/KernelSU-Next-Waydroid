// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/wait.h>

#include "feature/proc_modules_hide.h"
#include "hook/patch_memory.h"
#include "hook/syscall_hook_manager.h"
#include "infra/symbol_resolver.h"
#include "policy/allowlist.h"

typedef int (*seq_show_fn)(struct seq_file *m, void *v);

enum proc_modules_hook_state {
	PROC_MODULES_HOOK_INACTIVE,
	PROC_MODULES_HOOK_ACTIVE,
	PROC_MODULES_HOOK_DRAINING,
	PROC_MODULES_HOOK_DETACHED,
	PROC_MODULES_HOOK_FAILED,
};

static DEFINE_MUTEX(proc_modules_hook_lock);
static DECLARE_WAIT_QUEUE_HEAD(proc_modules_callback_waitq);
static atomic_t proc_modules_active_callbacks = ATOMIC_INIT(0);
static atomic_t proc_modules_state = ATOMIC_INIT(PROC_MODULES_HOOK_INACTIVE);
static struct seq_operations *modules_ops;
static seq_show_fn original_modules_show;
static bool modules_hook_installed;
static bool modules_filter_enabled;

static const char *proc_modules_state_name(int state)
{
	switch (state) {
	case PROC_MODULES_HOOK_INACTIVE:
		return "inactive";
	case PROC_MODULES_HOOK_ACTIVE:
		return "active";
	case PROC_MODULES_HOOK_DRAINING:
		return "draining";
	case PROC_MODULES_HOOK_DETACHED:
		return "detached";
	case PROC_MODULES_HOOK_FAILED:
		return "failed";
	default:
		return "invalid";
	}
}

static int ksu_proc_modules_state_get(char *buffer,
				      const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s callbacks=%d installed=%c\n",
			  proc_modules_state_name(atomic_read(&proc_modules_state)),
			  atomic_read(&proc_modules_active_callbacks),
			  READ_ONCE(modules_hook_installed) ? 'Y' : 'N');
}

static const struct kernel_param_ops ksu_proc_modules_state_ops = {
	.get = ksu_proc_modules_state_get,
};

module_param_cb(proc_modules_hook_state, &ksu_proc_modules_state_ops, NULL,
		0400);
MODULE_PARM_DESC(proc_modules_hook_state,
		 "Dynamic /proc/modules hook lifecycle and active callback count");

static bool ksu_hide_module_from_reader(const struct module *mod)
{
	uid_t uid;

	if (mod != THIS_MODULE || !READ_ONCE(modules_filter_enabled) ||
	    !ksu_is_waydroid_task(current))
		return false;

	uid = current_euid().val;
	return uid != 0 && !ksu_is_allow_uid_for_current(uid);
}

static int ksu_modules_show(struct seq_file *m, void *v)
{
	struct module *mod = list_entry(v, struct module, list);
	seq_show_fn original;
	int ret;

	/* The permanent unload guard is still held while this pointer is live.
	 * Take an additional reference for diagnostics and as a second barrier
	 * against an accidental unload while a callback is executing. */
	if (unlikely(!try_module_get(THIS_MODULE))) {
		original = READ_ONCE(original_modules_show);
		return original ? original(m, v) : -ENODEV;
	}

	atomic_inc(&proc_modules_active_callbacks);
	smp_mb__after_atomic();
	original = READ_ONCE(original_modules_show);

	if (unlikely(!original))
		ret = -ENODEV;
	else if (ksu_hide_module_from_reader(mod))
		ret = SEQ_SKIP;
	else
		ret = original(m, v);

	if (atomic_dec_and_test(&proc_modules_active_callbacks))
		wake_up_all(&proc_modules_callback_waitq);
	module_put(THIS_MODULE);
	return ret;
}

int ksu_proc_modules_hide_init(void)
{
	seq_show_fn replacement = ksu_modules_show;
	int ret = 0;

	if (!THIS_MODULE)
		return 0;

	mutex_lock(&proc_modules_hook_lock);
	if (modules_hook_installed) {
		ret = -EALREADY;
		goto out;
	}

	modules_ops = (struct seq_operations *)
		find_kernel_symbol_exact("modules_op");
	if (!modules_ops || !READ_ONCE(modules_ops->start) ||
	    !READ_ONCE(modules_ops->next) || !READ_ONCE(modules_ops->stop) ||
	    !READ_ONCE(modules_ops->show)) {
		pr_err("proc_modules_hide: modules_op is missing or invalid\n");
		ret = -ENOENT;
		goto failed;
	}

	original_modules_show = READ_ONCE(modules_ops->show);
	if (original_modules_show == replacement) {
		pr_err("proc_modules_hide: modules_op.show is already hooked\n");
		ret = -EALREADY;
		goto failed;
	}

	WRITE_ONCE(modules_filter_enabled, true);
	smp_wmb();
	ret = ksu_patch_text(&modules_ops->show, &replacement,
			     sizeof(replacement), KSU_PATCH_TEXT_FLUSH_DCACHE);
	if (ret || READ_ONCE(modules_ops->show) != replacement) {
		pr_err("proc_modules_hide: cannot patch modules_op.show: %d\n",
		       ret);
		if (!ret)
			ret = -EIO;
		WRITE_ONCE(modules_filter_enabled, false);
		goto failed;
	}

	WRITE_ONCE(modules_hook_installed, true);
	atomic_set(&proc_modules_state, PROC_MODULES_HOOK_ACTIVE);
	pr_info("proc_modules_hide: modules_op.show patched at %px (original %px)\n",
		&modules_ops->show, original_modules_show);
	goto out;

failed:
	atomic_set(&proc_modules_state, PROC_MODULES_HOOK_FAILED);
out:
	mutex_unlock(&proc_modules_hook_lock);
	return ret;
}

int ksu_proc_modules_hide_prepare_unload(void)
{
	seq_show_fn original;
	long drained;
	int ret = 0;

	if (!THIS_MODULE)
		return 0;

	mutex_lock(&proc_modules_hook_lock);
	if (atomic_read(&proc_modules_state) == PROC_MODULES_HOOK_DETACHED)
		goto out;

	atomic_set(&proc_modules_state, PROC_MODULES_HOOK_DRAINING);
	WRITE_ONCE(modules_filter_enabled, false);
	smp_mb();

	if (modules_hook_installed) {
		original = READ_ONCE(original_modules_show);
		if (!modules_ops || !original) {
			ret = -EINVAL;
			goto failed;
		}

		ret = ksu_patch_text(&modules_ops->show, &original,
				     sizeof(original),
				     KSU_PATCH_TEXT_FLUSH_DCACHE);
		if (ret || READ_ONCE(modules_ops->show) != original) {
			pr_emerg("proc_modules_hide: failed to restore modules_op.show: %d\n",
				 ret);
			if (!ret)
				ret = -EIO;
			goto failed;
		}
		WRITE_ONCE(modules_hook_installed, false);
		pr_emerg("proc_modules_hide: modules_op.show restored\n");
	}

	/* stop_machine() made the pointer replacement atomic, but a preempted
	 * task may already hold the old callback in a register. RCU Tasks waits
	 * for those tasks to pass a quiescent state before module text can go. */
	synchronize_rcu_tasks();

	drained = wait_event_timeout(proc_modules_callback_waitq,
			atomic_read(&proc_modules_active_callbacks) == 0,
			msecs_to_jiffies(2000));
	if (!drained && atomic_read(&proc_modules_active_callbacks) != 0) {
		pr_emerg("proc_modules_hide: %d callbacks did not drain\n",
			 atomic_read(&proc_modules_active_callbacks));
		ret = -EBUSY;
		goto failed;
	}

	/* Belt-and-suspenders barriers: drain ordinary RCU readers and retain a
	 * short grace delay matching the module's existing guarded-unload path. */
	synchronize_rcu();
	msleep(20);
	if (atomic_read(&proc_modules_active_callbacks) != 0) {
		ret = -EBUSY;
		goto failed;
	}

	atomic_set(&proc_modules_state, PROC_MODULES_HOOK_DETACHED);
	pr_emerg("proc_modules_hide: callback tasks and references drained\n");
	goto out;

failed:
	atomic_set(&proc_modules_state, PROC_MODULES_HOOK_DRAINING);
out:
	mutex_unlock(&proc_modules_hook_lock);
	return ret;
}

void ksu_proc_modules_hide_exit(void)
{
	int ret = ksu_proc_modules_hide_prepare_unload();

	if (ret)
		pr_emerg("proc_modules_hide: unsafe exit prevented earlier: %d\n",
			 ret);
}
