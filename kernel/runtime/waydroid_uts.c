// SPDX-License-Identifier: GPL-2.0
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/nsproxy.h>
#include <linux/string.h>
#include <linux/uts_namespace.h>
#include <linux/utsname.h>

#include "runtime/waydroid_uts.h"

/*
 * The host pre-start hook sets the desired release after loading this module
 * and before LXC can execute Android init.  It is intentionally not writable
 * after a Waydroid UTS has consumed it.
 */
static char requested_release[__NEW_UTS_LEN + 1];
static char requested_version[__NEW_UTS_LEN + 1];
static char original_release[__NEW_UTS_LEN + 1];
static char original_version[__NEW_UTS_LEN + 1];
static char applied_release[__NEW_UTS_LEN + 1];
static char applied_version[__NEW_UTS_LEN + 1];
static struct uts_namespace *applied_uts;
static DEFINE_MUTEX(waydroid_uts_lock);

static bool valid_release(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	size_t length = strnlen(value, sizeof(requested_release));

	if (!length || length > __NEW_UTS_LEN)
		return false;
	if (!isalnum(cursor[0]) ||
	    (!isalnum(cursor[length - 1]) && cursor[length - 1] != '+'))
		return false;
	for (; *cursor; cursor++) {
		if (!isalnum(*cursor) && *cursor != '.' && *cursor != '-' &&
		    *cursor != '_' && *cursor != '+')
			return false;
	}
	return true;
}

static bool valid_version(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	size_t length = strnlen(value, sizeof(requested_version));

	if (!length || length > __NEW_UTS_LEN)
		return false;
	for (; *cursor; cursor++) {
		if (!isprint(*cursor))
			return false;
	}
	return true;
}

static int set_requested_release(const char *value,
				 const struct kernel_param *kp)
{
	char candidate[sizeof(requested_release)];
	size_t length;
	int ret = 0;
	(void)kp;

	length = strcspn(value, "\n");
	if (length >= sizeof(candidate))
		return -ENAMETOOLONG;
	memcpy(candidate, value, length);
	candidate[length] = '\0';
	if (candidate[0] && !valid_release(candidate))
		return -EINVAL;

	mutex_lock(&waydroid_uts_lock);
	if (applied_uts) {
		ret = -EBUSY;
	} else {
		strscpy(requested_release, candidate, sizeof(requested_release));
	}
	mutex_unlock(&waydroid_uts_lock);
	return ret;
}

static int get_requested_release(char *buffer, const struct kernel_param *kp)
{
	int length;
	(void)kp;

	mutex_lock(&waydroid_uts_lock);
	length = scnprintf(buffer, sizeof(requested_release) + 1, "%s\n",
			   requested_release);
	mutex_unlock(&waydroid_uts_lock);
	return length;
}

static const struct kernel_param_ops waydroid_osrelease_ops = {
	.set = set_requested_release,
	.get = get_requested_release,
};

module_param_cb(waydroid_osrelease, &waydroid_osrelease_ops, NULL, 0600);
MODULE_PARM_DESC(waydroid_osrelease,
		 "One-shot osrelease for the isolated Waydroid UTS namespace");

static int set_requested_version(const char *value,
				 const struct kernel_param *kp)
{
	char candidate[sizeof(requested_version)];
	size_t length;
	int ret = 0;
	(void)kp;

	length = strcspn(value, "\n");
	if (length >= sizeof(candidate))
		return -ENAMETOOLONG;
	memcpy(candidate, value, length);
	candidate[length] = '\0';
	if (candidate[0] && !valid_version(candidate))
		return -EINVAL;

	mutex_lock(&waydroid_uts_lock);
	if (applied_uts)
		ret = -EBUSY;
	else
		strscpy(requested_version, candidate,
			sizeof(requested_version));
	mutex_unlock(&waydroid_uts_lock);
	return ret;
}

static int get_requested_version(char *buffer, const struct kernel_param *kp)
{
	int length;
	(void)kp;

	mutex_lock(&waydroid_uts_lock);
	length = scnprintf(buffer, sizeof(requested_version) + 1, "%s\n",
			   requested_version);
	mutex_unlock(&waydroid_uts_lock);
	return length;
}

static const struct kernel_param_ops waydroid_version_ops = {
	.set = set_requested_version,
	.get = get_requested_version,
};

module_param_cb(waydroid_version, &waydroid_version_ops, NULL, 0600);
MODULE_PARM_DESC(waydroid_version,
		 "One-shot version for the isolated Waydroid UTS namespace");

int ksu_waydroid_uts_apply_once(struct task_struct *task)
{
	struct uts_namespace *target;
	int ret = 0;

	if (!task->nsproxy || !task->nsproxy->uts_ns)
		return -ESRCH;
	target = task->nsproxy->uts_ns;
	if (target == &init_uts_ns) {
		pr_err("waydroid_uts: refused to modify the host UTS namespace\n");
		return -EPERM;
	}

	mutex_lock(&waydroid_uts_lock);
	if (!requested_release[0] && !requested_version[0])
		goto out;
	if (!requested_release[0] || !requested_version[0]) {
		pr_err("waydroid_uts: release and version must be configured together\n");
		ret = -EINVAL;
		goto out;
	}
	if (applied_uts) {
		ret = applied_uts == target ? -EALREADY : -EBUSY;
		goto out;
	}

	/* Keep the namespace alive until exit/unload restoration completes. The
	 * Waydroid init exit tracepoint normally releases this reference; the
	 * module-exit path is an idempotent fallback. */
	get_uts_ns(target);
	down_write(&uts_sem);
	strscpy(original_release, target->name.release,
		sizeof(original_release));
	strscpy(original_version, target->name.version,
		sizeof(original_version));
	strscpy(target->name.release, requested_release,
		sizeof(target->name.release));
	strscpy(target->name.version, requested_version,
		sizeof(target->name.version));
	strscpy(applied_release, requested_release,
		sizeof(applied_release));
	strscpy(applied_version, requested_version,
		sizeof(applied_version));
	up_write(&uts_sem);
	applied_uts = target;
	pr_info("waydroid_uts: one-shot kernel identity applied: %s / %s\n",
		applied_release, applied_version);
out:
	mutex_unlock(&waydroid_uts_lock);
	return ret;
}

void ksu_waydroid_uts_restore(void)
{
	struct uts_namespace *target = NULL;

	mutex_lock(&waydroid_uts_lock);
	if (!applied_uts)
		goto out;

	down_write(&uts_sem);
	strscpy(applied_uts->name.release, original_release,
		sizeof(applied_uts->name.release));
	strscpy(applied_uts->name.version, original_version,
		sizeof(applied_uts->name.version));
	up_write(&uts_sem);
	pr_info("waydroid_uts: restored kernel identity to %s / %s\n",
		original_release, original_version);
	target = applied_uts;
	applied_uts = NULL;
	original_release[0] = '\0';
	original_version[0] = '\0';
	applied_release[0] = '\0';
	applied_version[0] = '\0';
out:
	mutex_unlock(&waydroid_uts_lock);
	if (target)
		put_uts_ns(target);
}
