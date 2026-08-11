#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/jump_label.h"
#include "linux/printk.h"
#include "linux/seccomp.h"
#include "linux/sched/signal.h"
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
#include "infra/seccomp_cache.h"

static bool ksu_prepare_ksud_exec(const char __user **filename_user)
{
    char path[64];
    unsigned long addr;
    const char __user *fn;
    long ret;

    if (unlikely(!filename_user))
        return false;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    ret = strncpy_from_user(path, fn, sizeof(path));
    if (ret < 0)
        return false;

    path[sizeof(path) - 1] = '\0';
    if (strcmp(path, KSUD_PATH) != 0)
        return false;

#ifdef CONFIG_KSU_NON_ANDROID
    /*
     * Waydroid's seccomp policy kills reboot(2) with SIGSYS. KernelSU uses
     * that syscall as the bootstrap transport which installs its anonymous
     * driver fd.  The late-load helper is launched by `waydroid shell`, not
     * PID 1, so prepare whichever Android root task is executing KSUD_PATH.
     */
    if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
        current->seccomp.filter) {
        spin_lock_irq(&current->sighand->siglock);
        ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
        spin_unlock_irq(&current->sighand->siglock);
    }
#endif
    pr_info("hook_manager: prepare task %d executing ksud\n", current->pid);
    escape_to_root_for_init();
    return true;
}

static int ksu_handle_init_mark_tracker(const char __user **filename_user)
{
    char path[64];
    long ret;

    if (ksu_prepare_ksud_exec(filename_user))
        return 0;
    if (unlikely(!filename_user))
        return 0;

    ret = strncpy_from_user(path, *filename_user, sizeof(path));
    if (ret < 0)
        return 0;
    path[sizeof(path) - 1] = '\0';

    if (likely(strstr(path, "/app_process") == NULL && strstr(path, "/adbd") == NULL &&
               strstr(path, "/stub_zygote") == NULL)) {
        pr_info("hook_manager: unmark %d exec %s\n", current->pid, path);
        ksu_clear_task_tracepoint_flag_if_needed(current);
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

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
    const char __user **filename_user = (const char __user **)&PT_REGS_PARM1(regs);
    const char __user *const __user *argv_user = (const char __user *const __user *)PT_REGS_PARM2(regs);
#ifdef CONFIG_KSU_NON_ANDROID
    bool current_is_init = task_pid_vnr(current) == 1;
#else
    bool current_is_init = is_init(current_cred());
#endif
    struct ksu_sulog_pending_event *pending_root_execve = NULL;
    long ret;

    if (static_branch_unlikely(&ksud_execve_key))
        ksu_execve_hook_ksud(regs);

    if (current_euid().val == 0)
        pending_root_execve = ksu_sulog_capture_root_execve(*filename_user, argv_user, GFP_KERNEL);

#ifdef CONFIG_KSU_NON_ANDROID
    if (!current_is_init && current_euid().val == 0)
        ksu_prepare_ksud_exec(filename_user);

    if (current_is_init) {
#else
    if (current->pid != 1 && current_is_init) {
#endif
        ksu_handle_init_mark_tracker(filename_user);
        ret = ksu_adb_root_handle_execve((struct pt_regs *)regs);
        if (ret) {
            pr_err("adb root failed: %ld\n", ret);
        }
    } else if (ksu_su_compat_enabled) {
        ret = ksu_handle_execve_sucompat(filename_user, orig_nr, (struct pt_regs *)regs);
        ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
        return ret;
    }

    ret = ksu_syscall_table[orig_nr](regs);
    ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
    return ret;
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
