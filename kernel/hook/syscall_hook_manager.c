#include "linux/printk.h"
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/tracepoint.h>
#include <asm/syscall.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <trace/events/syscalls.h>
#ifdef CONFIG_KSU_NON_ANDROID
#include <trace/events/sched.h>
#include <linux/pid_namespace.h>
#endif

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
#include <linux/compat.h>
#include <linux/sched/task_stack.h>
#endif

#include "arch.h"
#include "ksu.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/syscall_hook_manager.h"
#include "hook/tp_marker.h"
#include "runtime/waydroid_uts.h"
#ifdef CONFIG_KSU_NON_ANDROID
#include "feature/proc_modules_hide.h"
#include "feature/overlayfs_statfs_hide.h"
#include "feature/proc_version_hide.h"
#endif
#include "feature/sucompat.h"
#include "hook/setuid_hook.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "policy/allowlist.h"
#include "runtime/ksud.h"

#ifdef CONFIG_KRETPROBES

static struct kretprobe *init_kretprobe(const char *name, kretprobe_handler_t handler)
{
    struct kretprobe *rp = kzalloc(sizeof(struct kretprobe), GFP_KERNEL);
    if (!rp)
        return NULL;
    rp->kp.symbol_name = name;
    rp->handler = handler;
    rp->data_size = 0;
    rp->maxactive = 0;

    int ret = register_kretprobe(rp);
    pr_info("hook_manager: register_%s kretprobe: %d\n", name, ret);
    if (ret) {
        kfree(rp);
        return NULL;
    }

    return rp;
}

static void destroy_kretprobe(struct kretprobe **rp_ptr)
{
    struct kretprobe *rp = *rp_ptr;
    if (!rp)
        return;
    unregister_kretprobe(rp);
    synchronize_rcu();
    kfree(rp);
    *rp_ptr = NULL;
}

static int syscall_regfunc_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    unsigned long flags;
    ksu_tp_marker_lock(&flags);
#ifdef CONFIG_KSU_NON_ANDROID
    /* Never use mark-all on a host kernel. */
    ksu_mark_running_process_locked();
#else
    if (ksu_tp_marker_reg_count() < 1) {
        // while install our tracepoint, mark our processes
        ksu_mark_running_process_locked();
    } else if (ksu_tp_marker_reg_count() == 1) {
        // while other tracepoint first added, mark all processes
        ksu_mark_all_process();
    }
#endif
    ksu_tp_marker_inc_reg_count();
    ksu_tp_marker_unlock(&flags);
    return 0;
}

static int syscall_unregfunc_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    unsigned long flags;
    ksu_tp_marker_lock(&flags);
    ksu_tp_marker_dec_reg_count();
#ifdef CONFIG_KSU_NON_ANDROID
    if (ksu_tp_marker_reg_count() <= 0)
        ksu_unmark_all_process();
    else
        ksu_mark_running_process_locked();
#else
    if (ksu_tp_marker_reg_count() <= 0) {
        // while no tracepoint left, unmark all processes
        ksu_unmark_all_process();
    } else if (ksu_tp_marker_reg_count() == 1) {
        // while just our tracepoint left, unmark disallowed processes
        ksu_mark_running_process_locked();
    }
#endif
    ksu_tp_marker_unlock(&flags);
    return 0;
}

static struct kretprobe *syscall_regfunc_rp = NULL;
static struct kretprobe *syscall_unregfunc_rp = NULL;
#endif

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
#ifdef CONFIG_KSU_NON_ANDROID
static bool waydroid_trace_enabled = true;
static pid_t waydroid_init_host_pid;
static struct pid_namespace *waydroid_pid_ns;
static DEFINE_SPINLOCK(waydroid_state_lock);
static bool sys_enter_registered;
static bool sched_exec_registered;
static bool sched_exit_registered;

bool ksu_is_waydroid_task(struct task_struct *task)
{
    struct pid_namespace *active_ns = task_active_pid_ns(task);
    bool matches;

    spin_lock(&waydroid_state_lock);
    matches = waydroid_trace_enabled && waydroid_pid_ns == active_ns;
    spin_unlock(&waydroid_state_lock);
    return matches;
}

static void ksu_waydroid_exec(void *data, struct task_struct *task,
                              pid_t old_pid, struct linux_binprm *bprm)
{
    const char *path = bprm->filename;

    /* This tracepoint runs only after exec has committed. Failed PATH probes
     * never arrive here and therefore cannot accidentally disable KernelSU
     * handling for the following /system/bin/su attempt. */
    if (task_active_pid_ns(task) == &init_pid_ns)
        return;

    if (task_pid_vnr(task) == 1 &&
        (!strcmp(path, "/init") || !strcmp(path, "/system/bin/init"))) {
        struct pid_namespace *old_ns;
        struct pid_namespace *new_ns = get_pid_ns(task_active_pid_ns(task));

        spin_lock(&waydroid_state_lock);
        old_ns = waydroid_pid_ns;
        waydroid_pid_ns = new_ns;
        waydroid_init_host_pid = task_pid_nr(task);
        waydroid_trace_enabled = true;
        spin_unlock(&waydroid_state_lock);
        if (old_ns)
            put_pid_ns(old_ns);
        ksu_set_task_tracepoint_flag(task);
        pr_info("hook_manager: Waydroid init started as host pid %d\n",
                waydroid_init_host_pid);
        if (ksu_waydroid_uts_apply_once(task) == -EPERM)
            pr_err("hook_manager: Waydroid UTS osrelease was not applied\n");
        return;
    }

    /* sched_process_exec is global. Ignore tasks which KernelSU did not mark,
     * both to preserve other tracepoint users and to avoid duplicate logs
     * after a task has already been removed from our syscall path. */
    if (!ksu_task_tracepoint_flag_is_set(task))
        return;

    /* These executables are roots of Android process trees or KernelSU's own
     * userspace. Their descendants must inherit syscall dispatch marking. */
    if (strstr(path, "/app_process") || strstr(path, "/adbd") ||
        strstr(path, "/stub_zygote") || !strcmp(path, KSUD_PATH))
        return;

    /* KernelSU WebUI forks a privileged /system/bin/sh which then invokes
     * /system/bin/su. Keep the dispatcher mark across that exec, but only for
     * root or explicitly allowlisted UIDs inside the registered Waydroid
     * namespace. Ordinary Android applications remain outside our syscall
     * path after exec. */
    if (task_uid(task).val == 0 || ksu_is_allow_uid(task_uid(task).val))
        return;

    ksu_clear_task_tracepoint_flag_if_needed(task);
    pr_info("hook_manager: unmark %d after successful exec %s\n",
            task->pid, path);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static void ksu_waydroid_exit(void *data, struct task_struct *task,
                              bool group_dead)
#else
static void ksu_waydroid_exit(void *data, struct task_struct *task)
#endif
{
    struct pid_namespace *old_ns;

    if (task_pid_nr(task) != waydroid_init_host_pid)
        return;

    spin_lock(&waydroid_state_lock);
    waydroid_trace_enabled = false;
    waydroid_init_host_pid = 0;
    old_ns = waydroid_pid_ns;
    waydroid_pid_ns = NULL;
    spin_unlock(&waydroid_state_lock);
    if (old_ns)
        put_pid_ns(old_ns);
    ksu_waydroid_uts_restore();
    pr_info("hook_manager: Waydroid init stopped\n");
}
#else
bool ksu_is_waydroid_task(struct task_struct *task)
{
    return false;
}
#endif

// sys_enter handler: redirect hooked syscalls to the dispatcher
static void ksu_sys_enter_handler(void *data, struct pt_regs *regs, long id)
{
    if (unlikely(ksu_syscall_unload_prepared()))
        return;
#ifdef CONFIG_KSU_NON_ANDROID
    if (unlikely(!ksu_is_waydroid_task(current)))
        return;
#endif
#if defined(__x86_64__)
    if (unlikely(in_compat_syscall()))
#elif defined(__aarch64__)
    if (unlikely(is_compat_task()))
#endif
        return;

    if (ksu_dispatcher_nr < 0)
        return;

    if (ksu_has_syscall_hook(id)) {
        struct pt_regs *current_regs = task_pt_regs(current);

#if defined(__x86_64__)
        // Stash the original syscall number in ax.
        // We use ax because it currently just holds -ENOSYS and is safe to overwrite.
        current_regs->ax = id;
        current_regs->orig_ax = ksu_dispatcher_nr;
#elif defined(__aarch64__)
        PT_REGS_ORIG_SYSCALL(current_regs) = id;
        current_regs->syscallno = ksu_dispatcher_nr;
#endif
    }
}
#endif

void __init ksu_syscall_hook_manager_init(void)
{
    int ret;
    pr_info("hook_manager: ksu_hook_manager_init called\n");

#ifdef CONFIG_KRETPROBES
    syscall_regfunc_rp = init_kretprobe("syscall_regfunc", syscall_regfunc_handler);
    syscall_unregfunc_rp = init_kretprobe("syscall_unregfunc", syscall_unregfunc_handler);
#endif

    // Register syscall hooks via dispatcher
    ksu_register_syscall_hook(__NR_setresuid, ksu_hook_setresuid);
    ksu_register_syscall_hook(__NR_execve, ksu_hook_execve);
    ksu_register_syscall_hook(__NR_execveat, ksu_hook_execveat);
    ksu_register_syscall_hook(__NR_newfstatat, ksu_hook_newfstatat);
    ksu_register_syscall_hook(__NR_faccessat, ksu_hook_faccessat);
    ksu_register_syscall_hook(__NR_reboot, ksu_hook_reboot);

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
    ret = register_trace_prio_sys_enter(ksu_sys_enter_handler, NULL, INT_MIN);
#ifndef CONFIG_KRETPROBES
    ksu_mark_running_process_locked();
#endif
    if (ret) {
        pr_err("hook_manager: failed to register sys_enter tracepoint: %d\n", ret);
    } else {
#ifdef CONFIG_KSU_NON_ANDROID
        sys_enter_registered = true;
#endif
        pr_info("hook_manager: sys_enter tracepoint registered\n");
    }
#ifdef CONFIG_KSU_NON_ANDROID
    ret = register_trace_sched_process_exec(ksu_waydroid_exec, NULL);
    if (!ret)
        sched_exec_registered = true;
    else
        pr_err("hook_manager: failed Waydroid exec tracepoint: %d\n", ret);

    ret = register_trace_sched_process_exit(ksu_waydroid_exit, NULL);
    if (!ret)
        sched_exit_registered = true;
    else
        pr_err("hook_manager: failed Waydroid exit tracepoint: %d\n", ret);
#endif
#endif

#ifdef CONFIG_KSU_NON_ANDROID
    ksu_proc_modules_hide_init();
    ksu_overlayfs_statfs_hide_init();
    ksu_proc_version_hide_init();
#endif
    ksu_setuid_hook_init();
    ksu_sucompat_init();
    ksu_avc_spoof_init();
}

void __exit ksu_syscall_hook_manager_exit(void)
{
    pr_emerg("exit: hook manager begin\n");
#ifdef CONFIG_KSU_NON_ANDROID
    spin_lock(&waydroid_state_lock);
    waydroid_trace_enabled = false;
    spin_unlock(&waydroid_state_lock);

    /* Remove raw callback pointers before any module-owned state is torn down. */
    ksu_proc_version_hide_exit();
    ksu_overlayfs_statfs_hide_exit();
    ksu_proc_modules_hide_exit();
#endif

#ifdef CONFIG_KRETPROBES
    /* unregister_trace_* invokes syscall_unregfunc(). Remove our observers
     * first so teardown cannot re-enter task marking while unregistering. */
    destroy_kretprobe(&syscall_regfunc_rp);
    destroy_kretprobe(&syscall_unregfunc_rp);
    pr_emerg("exit: tracepoint observer kretprobes removed\n");
#endif

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
#ifdef CONFIG_KSU_NON_ANDROID
    if (sys_enter_registered) {
        unregister_trace_sys_enter(ksu_sys_enter_handler, NULL);
        sys_enter_registered = false;
    }
    if (sched_exec_registered) {
        unregister_trace_sched_process_exec(ksu_waydroid_exec, NULL);
        sched_exec_registered = false;
    }
    if (sched_exit_registered) {
        unregister_trace_sched_process_exit(ksu_waydroid_exit, NULL);
        sched_exit_registered = false;
    }
#else
    unregister_trace_sys_enter(ksu_sys_enter_handler, NULL);
#endif
    tracepoint_synchronize_unregister();
    pr_emerg("exit: tracepoints removed and synchronized\n");
#endif

    ksu_waydroid_uts_restore();

#ifdef CONFIG_KSU_NON_ANDROID
    {
        struct pid_namespace *old_ns;

        spin_lock(&waydroid_state_lock);
        waydroid_init_host_pid = 0;
        old_ns = waydroid_pid_ns;
        waydroid_pid_ns = NULL;
        spin_unlock(&waydroid_state_lock);
        if (old_ns)
            put_pid_ns(old_ns);
    }
#endif

    ksu_unregister_syscall_hook(__NR_setresuid);
    ksu_unregister_syscall_hook(__NR_execve);
    ksu_unregister_syscall_hook(__NR_execveat);
    ksu_unregister_syscall_hook(__NR_newfstatat);
    ksu_unregister_syscall_hook(__NR_faccessat);

    ksu_syscall_hook_exit();
    pr_emerg("exit: syscall dispatcher restored\n");

    ksu_sucompat_exit();
    ksu_setuid_hook_exit();
    ksu_avc_spoof_exit();
    pr_emerg("exit: hook manager complete\n");
}
