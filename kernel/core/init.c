#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "policy/allowlist.h"
#include "policy/app_profile.h"
#include "policy/feature.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_observer.h"
#include "manager/throne_tracker.h"
#include "hook/syscall_hook_manager.h"
#include "hook/lsm_hook.h"
#include "runtime/ksud.h"
#include "runtime/ksud_boot.h"
#include "supercall/supercall.h"
#include "ksu.h"
#include "infra/file_wrapper.h"
#include "selinux/selinux.h"
#include "hook/syscall_hook.h"
#include "feature/adb_root.h"
#include "feature/selinux_hide.h"
#include "feature/sulog.h"
#include "feature/susfs_bridge.h"
#ifdef CONFIG_KSU_NON_ANDROID
#include "feature/proc_modules_hide.h"
#include "feature/overlayfs_statfs_hide.h"
#include "feature/proc_version_hide.h"
#endif
#include "infra/symbol_resolver.h"

#if defined(__x86_64__) && !defined(CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER)
#include <asm/cpufeature.h>
#include <linux/version.h>
#ifndef X86_FEATURE_INDIRECT_SAFE
#error "FATAL: Your kernel is missing the indirect syscall bypass patches!"
#endif
#endif

// workaround for A12-5.10 kernel
// Some third-party kernel (e.g. linegaeOS) uses wrong toolchain, which supports
// CC_HAVE_STACKPROTECTOR_SYSREG while gki's toolchain doesn't.
// Therefore, ksu lkm, which uses gki toolchain, requires this __stack_chk_guard,
// while those third-party kernel can't provide.
// Thus, we manually provide it instead of using kernel's
#if defined(CONFIG_STACKPROTECTOR) &&                                          \
    (defined(CONFIG_ARM64) && defined(MODULE) &&                               \
     !defined(CONFIG_STACKPROTECTOR_PER_TASK))
#include <linux/stackprotector.h>
#include <linux/random.h>
unsigned long __stack_chk_guard __ro_after_init
    __attribute__((visibility("hidden")));

__attribute__((no_stack_protector)) void __init ksu_setup_stack_chk_guard()
{
    unsigned long canary;

    /* Try to get a semi random initial value. */
    get_random_bytes(&canary, sizeof(canary));
    canary ^= LINUX_VERSION_CODE;
    canary &= CANARY_MASK;
    __stack_chk_guard = canary;
}

__attribute__((naked)) int __init kernelsu_init_early(void)
{
    asm("mov x19, x30;\n"
        "bl ksu_setup_stack_chk_guard;\n"
        "mov x30, x19;\n"
        "b kernelsu_init;\n");
}
#define NEED_OWN_STACKPROTECTOR 1
#else
#define NEED_OWN_STACKPROTECTOR 0
#endif

struct cred *ksu_cred;
bool ksu_late_loaded;

#ifdef CONFIG_KSU_DEBUG
bool allow_shell = true;
#else
bool allow_shell = false;
#endif
module_param(allow_shell, bool, 0);

bool ksu_no_custom_rc = false;
module_param_named(norc, ksu_no_custom_rc, bool, 0);

#if defined(MODULE) && defined(__x86_64__) &&                         \
    defined(CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER)
static bool unload_guard_held;
static bool prepare_unload;

static int ksu_param_set_prepare_unload(const char *val,
                                        const struct kernel_param *kp)
{
    bool requested;
    int ret;

    ret = kstrtobool(val, &requested);
    if (ret)
        return ret;
    if (!requested)
        return -EINVAL;
    if (READ_ONCE(prepare_unload))
        return 0;

#ifdef CONFIG_KSU_NON_ANDROID
    /* Detach and drain every global callback while the permanent
     * self-reference is still held. */
    ret = ksu_proc_version_hide_prepare_unload();
    if (ret)
        return ret;

    ret = ksu_overlayfs_statfs_hide_prepare_unload();
    if (ret)
        return ret;

    ret = ksu_proc_modules_hide_prepare_unload();
    if (ret)
        return ret;
#endif

    ret = ksu_syscall_hook_prepare_unload();
    if (ret)
        return ret;

	/* Raw syscall-table function pointers do not pin their owner.  The x86
	 * wrappers do so explicitly; after detaching every entry point, wait for
	 * short in-flight calls and refuse to drop our final guard if any sleeping
	 * wrapper still has a return address in module text. */
    synchronize_rcu_tasks();
    for (ret = 0; ret < 200 && module_refcount(THIS_MODULE) > 1; ret++)
        msleep(10);
    if (module_refcount(THIS_MODULE) > 1) {
        pr_emerg("prepare unload: %u module references still active\n",
                 module_refcount(THIS_MODULE) - 1);
        return -EBUSY;
    }

    WRITE_ONCE(prepare_unload, true);

    /* This is the sole self-reference acquired after successful init.  Drop
     * it only after all global x86 entry points have been restored.  The
     * sysfs write returns completely to userspace before delete_module is
     * issued by the loader helper. */
    if (xchg(&unload_guard_held, false))
        module_put(THIS_MODULE);

    pr_emerg("prepare unload: module guard released\n");
    return 0;
}

static int ksu_param_get_prepare_unload(char *buffer,
                                        const struct kernel_param *kp)
{
    return sysfs_emit(buffer, "%c\n",
                      READ_ONCE(prepare_unload) ? 'Y' : 'N');
}

static const struct kernel_param_ops ksu_prepare_unload_ops = {
    .set = ksu_param_set_prepare_unload,
    .get = ksu_param_get_prepare_unload,
};

module_param_cb(prepare_unload, &ksu_prepare_unload_ops, NULL, 0600);
MODULE_PARM_DESC(prepare_unload,
                 "Detach and drain global kernel callbacks before unloading");
#endif

int __init kernelsu_init(void)
{
#if defined(__x86_64__) && !defined(CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER)
    // If the kernel has the hardening patch, X86_FEATURE_INDIRECT_SAFE must be set
    if (!boot_cpu_has(X86_FEATURE_INDIRECT_SAFE)) {
        pr_alert("*************************************************************");
        pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
        pr_alert("**                                                         **");
        pr_alert("**        X86_FEATURE_INDIRECT_SAFE is not enabled!        **");
        pr_alert("**      KernelSU will abort initialization to prevent      **");
        pr_alert("**                     kernel panic.                       **");
        pr_alert("**                                                         **");
        pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
        pr_alert("*************************************************************");
        return -ENOSYS;
    }
#endif

#if defined(MODULE) && !defined(CONFIG_KSU_NON_ANDROID)
	ksu_late_loaded = (current->pid != 1);
#else
	ksu_late_loaded = false;
#endif

#ifdef CONFIG_KSU_DEBUG
	pr_alert("*************************************************************");
	pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
	pr_alert("**                                                         **");
	pr_alert("**         You are running KernelSU in DEBUG mode          **");
	pr_alert("**                                                         **");
	pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
	pr_alert("*************************************************************");
#endif
	if (allow_shell) {
		pr_alert("shell is allowed at init!");
	}

	ksu_cred = prepare_creds();
	if (!ksu_cred) {
		pr_err("prepare cred failed!\n");
		return -ENOSYS;
	}

	if (ksu_susfs_bridge_init()) {
		put_cred(ksu_cred);
		return -ENODEV;
	}

	ksu_init_symbol_resolver();
	ksu_syscall_hook_init();

	ksu_feature_init();

	ksu_sulog_init();

	ksu_adb_root_init();

	ksu_lsm_hook_init();

#ifdef CONFIG_KSU_SELINUX
	ksu_selinux_hide_init();
#endif

	ksu_supercalls_init();

	if (ksu_late_loaded) {
		pr_info("late load mode, skipping kprobe hooks\n");

#ifdef CONFIG_KSU_SELINUX
		apply_kernelsu_rules();
		cache_sid();
		setup_ksu_cred();
#endif

		// Grant current process (ksud late-load) root
		// with KSU SELinux domain before enforcing SELinux, so it
		// can continue to access /data/app etc. after enforcement.
#ifdef CONFIG_KSU_SELINUX
		escape_to_root_for_init();
#endif

		ksu_allowlist_init();
		ksu_load_allow_list();

		ksu_syscall_hook_manager_init();

		ksu_throne_tracker_init();
		ksu_observer_init();
		ksu_file_wrapper_init();

		ksu_boot_completed = true;
		track_throne(false);

#ifdef CONFIG_KSU_SELINUX
		if (!getenforce()) {
			pr_info("Permissive SELinux, enforcing\n");
			setenforce(true);
		}
#endif

	} else {
		ksu_syscall_hook_manager_init();

		ksu_allowlist_init();

		ksu_throne_tracker_init();

		ksu_ksud_init();

		ksu_file_wrapper_init();
	}

#ifdef MODULE
#if !defined(CONFIG_KSU_DEBUG) && !defined(CONFIG_KSU_NON_ANDROID)
	kobject_del(&THIS_MODULE->mkobj.kobj);
#endif
#endif

#if defined(MODULE) && defined(__x86_64__) &&                         \
    defined(CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER)
    /* Raw rmmod must fail safely until userspace explicitly detaches the
     * global dispatcher through prepare_unload. */
    __module_get(THIS_MODULE);
    WRITE_ONCE(unload_guard_held, true);
    pr_info("x86 unload guard armed\n");
#endif
    return 0;
}

void __exit kernelsu_exit(void)
{
	pr_emerg("exit: KernelSU teardown begin\n");
	ksu_susfs_bridge_exit();
	// Phase 1: Stop all hooks first to prevent new callbacks
	ksu_syscall_hook_manager_exit();

	ksu_supercalls_exit();
	pr_emerg("exit: supercalls removed\n");

	if (!ksu_late_loaded)
		ksu_ksud_exit();
	pr_emerg("exit: ksud hooks and work drained\n");

	// Wait for any in-flight RCU readers (e.g. handler traversing allow_list)
	synchronize_rcu();
	pr_emerg("exit: RCU readers drained\n");
	/* Syscall, kprobe and task callbacks may have observed module-owned
	 * function pointers before their entry points were unregistered. */
	synchronize_rcu_tasks();
	pr_emerg("exit: RCU tasks drained\n");

	// Phase 2: Now safe to release data structures
	ksu_observer_exit();

	ksu_throne_tracker_exit();

	ksu_allowlist_exit();

#ifdef CONFIG_KSU_SELINUX
	ksu_selinux_hide_exit();
#endif

	ksu_lsm_hook_exit();

	ksu_adb_root_exit();

	ksu_sulog_exit();

	ksu_feature_exit();

	put_cred(ksu_cred);
	pr_emerg("exit: KernelSU teardown complete\n");
}

#if NEED_OWN_STACKPROTECTOR
module_init(kernelsu_init_early);
#else
module_init(kernelsu_init);
#endif
module_exit(kernelsu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("weishu");
MODULE_DESCRIPTION("Android KernelSU");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
