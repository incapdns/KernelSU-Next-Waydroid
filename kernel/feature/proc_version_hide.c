// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "feature/proc_version_hide.h"
#include "hook/syscall_hook_manager.h"
#include "policy/allowlist.h"

enum proc_version_hook_state {
	PROC_VERSION_HOOK_INACTIVE,
	PROC_VERSION_HOOK_ACTIVE,
	PROC_VERSION_HOOK_DRAINING,
	PROC_VERSION_HOOK_DETACHED,
	PROC_VERSION_HOOK_FAILED,
};

struct proc_version_instance_data {
	struct seq_file *seq;
	bool module_ref;
};

static DEFINE_MUTEX(proc_version_hook_lock);
static DECLARE_WAIT_QUEUE_HEAD(proc_version_callback_waitq);
static atomic_t proc_version_active_callbacks = ATOMIC_INIT(0);
static atomic_t proc_version_state = ATOMIC_INIT(PROC_VERSION_HOOK_INACTIVE);
static struct kretprobe proc_version_rp;
static bool proc_version_probe_registered;
static bool proc_version_filter_enabled;

static const char *proc_version_state_name(int state)
{
	switch (state) {
	case PROC_VERSION_HOOK_INACTIVE:
		return "inactive";
	case PROC_VERSION_HOOK_ACTIVE:
		return "active";
	case PROC_VERSION_HOOK_DRAINING:
		return "draining";
	case PROC_VERSION_HOOK_DETACHED:
		return "detached";
	case PROC_VERSION_HOOK_FAILED:
		return "failed";
	default:
		return "invalid";
	}
}

static int ksu_proc_version_state_get(char *buffer,
				      const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s callbacks=%d registered=%c missed=%d\n",
			  proc_version_state_name(atomic_read(&proc_version_state)),
			  atomic_read(&proc_version_active_callbacks),
			  READ_ONCE(proc_version_probe_registered) ? 'Y' : 'N',
			  READ_ONCE(proc_version_rp.nmissed));
}

static const struct kernel_param_ops ksu_proc_version_state_ops = {
	.get = ksu_proc_version_state_get,
};

module_param_cb(proc_version_hook_state, &ksu_proc_version_state_ops, NULL,
		0400);
MODULE_PARM_DESC(proc_version_hook_state,
		 "Per-reader /proc/version hook lifecycle and callback count");

static bool ksu_should_hide_proc_version(void)
{
	uid_t uid;

	if (!READ_ONCE(proc_version_filter_enabled) ||
	    !ksu_is_waydroid_task(current))
		return false;

	uid = current_euid().val;
	return uid != 0 && !ksu_is_allow_uid_for_current(uid);
}

static void ksu_sanitize_proc_version(struct seq_file *m)
{
	static const char replacement[] = "android-build";
	char *open;
	char *close;
	char *at;
	size_t inner_len;
	size_t tail_len;
	size_t replacement_len = sizeof(replacement) - 1;

	if (!m || !m->buf || !m->count)
		return;

	/* linux_proc_banner places the compiler identity in the first pair of
	 * parentheses.  Hide the host account there while retaining the UTS
	 * release/version already virtualized for this Waydroid namespace. */
	open = memchr(m->buf, '(', m->count);
	if (!open)
		return;
	close = memchr(open + 1, ')', m->count - (open + 1 - m->buf));
	if (!close)
		return;

	inner_len = close - open - 1;
	if (inner_len >= replacement_len) {
		tail_len = m->count - (close - m->buf);
		memcpy(open + 1, replacement, replacement_len);
		memmove(open + 1 + replacement_len, close, tail_len);
		m->count -= inner_len - replacement_len;
		if (m->count < m->size)
			m->buf[m->count] = '\0';
		return;
	}

	/* Never expand seq_file storage in the unlikely case of a shorter build
	 * identity. Removing '@' is sufficient to avoid exposing host@builder. */
	at = memchr(open + 1, '@', inner_len);
	if (at)
		*at = '_';
}

static int ksu_proc_version_entry(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	struct proc_version_instance_data *data =
		(struct proc_version_instance_data *)ri->data;

	data->seq = NULL;
	data->module_ref = false;
	if (!ksu_should_hide_proc_version())
		return 1;
	if (unlikely(!try_module_get(THIS_MODULE)))
		return 1;

	data->module_ref = true;
	data->seq = (struct seq_file *)regs_get_kernel_argument(regs, 0);
	atomic_inc(&proc_version_active_callbacks);
	smp_mb__after_atomic();
	return 0;
}

static int ksu_proc_version_return(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct proc_version_instance_data *data =
		(struct proc_version_instance_data *)ri->data;

	if (data->module_ref) {
		ksu_sanitize_proc_version(data->seq);
		if (atomic_dec_and_test(&proc_version_active_callbacks))
			wake_up_all(&proc_version_callback_waitq);
		module_put(THIS_MODULE);
	}
	return 0;
}

int ksu_proc_version_hide_init(void)
{
	int ret = 0;

	if (!THIS_MODULE)
		return 0;

	mutex_lock(&proc_version_hook_lock);
	if (proc_version_probe_registered) {
		ret = -EALREADY;
		goto out;
	}

	memset(&proc_version_rp, 0, sizeof(proc_version_rp));
	proc_version_rp.kp.symbol_name = "version_proc_show";
	proc_version_rp.entry_handler = ksu_proc_version_entry;
	proc_version_rp.handler = ksu_proc_version_return;
	proc_version_rp.data_size = sizeof(struct proc_version_instance_data);
	proc_version_rp.maxactive = 64;

	WRITE_ONCE(proc_version_filter_enabled, true);
	ret = register_kretprobe(&proc_version_rp);
	if (ret) {
		WRITE_ONCE(proc_version_filter_enabled, false);
		atomic_set(&proc_version_state, PROC_VERSION_HOOK_FAILED);
		pr_warn("proc_version_hide: cannot probe version_proc_show: %d\n",
			ret);
		goto out;
	}

	WRITE_ONCE(proc_version_probe_registered, true);
	atomic_set(&proc_version_state, PROC_VERSION_HOOK_ACTIVE);
	pr_info("proc_version_hide: per-reader filter registered\n");
out:
	mutex_unlock(&proc_version_hook_lock);
	return ret;
}

int ksu_proc_version_hide_prepare_unload(void)
{
	long drained;
	int ret = 0;

	if (!THIS_MODULE)
		return 0;

	mutex_lock(&proc_version_hook_lock);
	if (atomic_read(&proc_version_state) == PROC_VERSION_HOOK_DETACHED)
		goto out;

	atomic_set(&proc_version_state, PROC_VERSION_HOOK_DRAINING);
	WRITE_ONCE(proc_version_filter_enabled, false);
	smp_mb();

	if (proc_version_probe_registered) {
		unregister_kretprobe(&proc_version_rp);
		WRITE_ONCE(proc_version_probe_registered, false);
		pr_emerg("proc_version_hide: kretprobe unregistered (missed=%d)\n",
			 proc_version_rp.nmissed);
	}

	/* unregister_kretprobe() drains registered instances. Keep the same
	 * explicit task-RCU, callback-counter and delay barriers as the raw
	 * function-table hooks so module unload has no weaker path. */
	synchronize_rcu_tasks();
	drained = wait_event_timeout(proc_version_callback_waitq,
			atomic_read(&proc_version_active_callbacks) == 0,
			msecs_to_jiffies(2000));
	if (!drained && atomic_read(&proc_version_active_callbacks) != 0) {
		ret = -EBUSY;
		goto out;
	}
	synchronize_rcu();
	msleep(20);
	if (atomic_read(&proc_version_active_callbacks) != 0) {
		ret = -EBUSY;
		goto out;
	}

	atomic_set(&proc_version_state, PROC_VERSION_HOOK_DETACHED);
	pr_emerg("proc_version_hide: callback tasks and references drained\n");
out:
	mutex_unlock(&proc_version_hook_lock);
	return ret;
}

void ksu_proc_version_hide_exit(void)
{
	int ret = ksu_proc_version_hide_prepare_unload();

	if (ret)
		pr_emerg("proc_version_hide: unsafe exit prevented earlier: %d\n",
			 ret);
}
