#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/jump_label.h"
#include "linux/printk.h"
#include "linux/string.h"
#include "selinux/selinux.h"
#include <asm/syscall.h>
#include <linux/ptrace.h>
#include <linux/static_key.h>

#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/tp_marker.h"
#include "feature/sucompat.h"
#include "hook/setuid_hook.h"
#include "policy/app_profile.h"
#include "runtime/ksud.h"
#include "sulog/event.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "feature/adb_root.h"
#include "supercall/supercall.h"
#include "feature/susfs_bridge.h"
#ifdef CONFIG_KSU_SUSFS
#include <linux/susfs_def.h>
#include "uapi/supercall.h"
#endif

static int ksu_handle_init_mark_tracker(const char __user **filename_user)
{
    char path[64];
    unsigned long addr;
    const char __user *fn;
    long ret;

    if (unlikely(!filename_user))
        return 0;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    ret = strncpy_from_user(path, fn, sizeof(path));
    if (ret < 0)
        return 0;

    path[sizeof(path) - 1] = '\0';
    if (unlikely(strcmp(path, KSUD_PATH) == 0)) {
        pr_info("hook_manager: escape to root for init executing ksud: %d\n", current->pid);
        escape_to_root_for_init();
#ifdef CONFIG_KSU_NON_ANDROID
        /* Waydroid's init inherits the host LXC seccomp filter, which blocks
         * the reboot(2) bootstrap transport. Install an inheritable driver fd
         * before exec so ksud never needs that syscall. */
        if (ksu_install_fd_for_exec() < 0)
            pr_err("hook_manager: failed to install inheritable ksud fd\n");
#endif
#ifndef CONFIG_KSU_NON_ANDROID
    } else if (likely(strstr(path, "/app_process") == NULL && strstr(path, "/adbd") == NULL &&
                      strstr(path, "/stub_zygote") == NULL)) {
        pr_info("hook_manager: unmark %d exec %s\n", current->pid, path);
        ksu_clear_task_tracepoint_flag_if_needed(current);
#endif
    }

    return 0;
}

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
    if (!ksu_su_compat_enabled)
        return ksu_syscall_table[orig_nr](regs);

    return ksu_handle_stat_sucompat(orig_nr, (struct pt_regs *)regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
    if (!ksu_su_compat_enabled)
        return ksu_syscall_table[orig_nr](regs);

    return ksu_handle_faccessat_sucompat(orig_nr, (struct pt_regs *)regs);
}

DEFINE_STATIC_KEY_TRUE(ksud_execve_key);

void ksu_stop_ksud_execve_hook()
{
    static_branch_disable(&ksud_execve_key);
}

static long __nocfi ksu_hook_execve_common(int orig_nr, const struct pt_regs *regs, bool execveat)
{
    const char __user **filename_user =
        execveat ? (const char __user **)&PT_REGS_PARM2(regs) : (const char __user **)&PT_REGS_PARM1(regs);
    const char __user *const __user *argv_user = execveat ? (const char __user *const __user *)PT_REGS_PARM3(regs) :
                                                            (const char __user *const __user *)PT_REGS_PARM2(regs);
#ifdef CONFIG_KSU_NON_ANDROID
    bool current_is_init = task_pid_vnr(current) == 1;
#else
    bool current_is_init = is_init(current_cred());
#endif
    struct ksu_sulog_pending_event *pending_root_execve = NULL;
    long ret;

    if (static_branch_unlikely(&ksud_execve_key)) {
        if (execveat) {
            ksu_execveat_hook_ksud(regs);
        } else {
            ksu_execve_hook_ksud(regs);
        }
    }

#ifdef CONFIG_KSU_NON_ANDROID
    /* Android init forks an exec-service child before executing ksud, so the
     * exec task is no longer PID 1. The dispatcher already limits privileged
     * handling to tasks marked as belonging to the Waydroid PID namespace. */
    ksu_handle_init_mark_tracker(filename_user);
#endif

    if (current_euid().val == 0)
        pending_root_execve = ksu_sulog_capture_root_execve(*filename_user, argv_user, GFP_KERNEL);

#ifdef CONFIG_KSU_NON_ANDROID
    if (current_is_init) {
#else
    if (current->pid != 1 && current_is_init) {
#endif
#ifndef CONFIG_KSU_NON_ANDROID
        ksu_handle_init_mark_tracker(filename_user);
#endif
        ret = execveat ? ksu_adb_root_handle_execveat((struct pt_regs *)regs) :
                         ksu_adb_root_handle_execve((struct pt_regs *)regs);
        if (ret) {
            pr_err("adb root failed: %ld\n", ret);
        }
    } else if (ksu_su_compat_enabled) {
        ret = execveat ? ksu_handle_execveat_sucompat(filename_user, orig_nr, (struct pt_regs *)regs) :
                         ksu_handle_execve_sucompat(filename_user, orig_nr, (struct pt_regs *)regs);
        ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
        return ret;
    }

    ret = ksu_syscall_table[orig_nr](regs);
    ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
    return ret;
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
    return ksu_hook_execve_common(orig_nr, regs, false);
}

long __nocfi ksu_hook_execveat(int orig_nr, const struct pt_regs *regs)
{
    return ksu_hook_execve_common(orig_nr, regs, true);
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
    uid_t old_uid = current_uid().val;
    long ret = ksu_syscall_table[orig_nr](regs);

    if (ret < 0)
        return ret;

    ksu_handle_setresuid(old_uid, current_uid().val);
    return ret;
}

long __nocfi ksu_hook_reboot(int orig_nr, const struct pt_regs *regs)
{
#ifdef CONFIG_KSU_SUSFS
	int magic1 = (int)PT_REGS_PARM1(regs);
	int magic2 = (int)PT_REGS_PARM2(regs);
	unsigned int cmd = (unsigned int)PT_REGS_PARM3(regs);
	unsigned long arg = (unsigned long)PT_REGS_SYSCALL_PARM4(regs);

	if (magic1 == KSU_INSTALL_MAGIC1 && magic2 == SUSFS_MAGIC &&
	    current_uid().val == 0)
		return ksu_susfs_bridge_dispatch(cmd, arg);
#endif
	return ksu_syscall_table[orig_nr](regs);
}
