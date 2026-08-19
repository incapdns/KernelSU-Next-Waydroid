#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "uapi/supercall.h"
#include "supercall/internal.h"
#include "arch.h"
#include "util.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"

#include "sulog/event.h"

uint32_t ksuver_override = 0;

struct ksu_install_fd_tw {
    struct callback_head cb;
    int __user *outp;
};

static int anon_ksu_release(struct inode *inode, struct file *filp)
{
    pr_info("ksu fd released\n");
    return 0;
}

static long anon_ksu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return ksu_supercall_handle_ioctl(cmd, (void __user *)arg);
}

static const struct file_operations anon_ksu_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = anon_ksu_ioctl,
    .compat_ioctl = anon_ksu_ioctl,
    .release = anon_ksu_release,
};

static int ksu_install_fd_with_flags(unsigned int flags)
{
    struct file *filp;
    int fd;

	fd = get_unused_fd_flags(flags);
    if (fd < 0) {
        pr_err("ksu_install_fd: failed to get unused fd\n");
        return fd;
    }

	filp = anon_inode_getfile("[ksu_driver]", &anon_ksu_fops, NULL, O_RDWR | flags);
    if (IS_ERR(filp)) {
        pr_err("ksu_install_fd: failed to create anon inode file\n");
        put_unused_fd(fd);
        return PTR_ERR(filp);
    }

    fd_install(fd, filp);
    pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);
	return fd;
}

int ksu_install_fd(void)
{
	return ksu_install_fd_with_flags(O_CLOEXEC);
}

int ksu_install_fd_for_exec(void)
{
	return ksu_install_fd_with_flags(0);
}

static void ksu_install_fd_tw_func(struct callback_head *cb)
{
    struct ksu_install_fd_tw *tw = container_of(cb, struct ksu_install_fd_tw, cb);
    int fd = ksu_install_fd();

    pr_info("[%d] install ksu fd: %d\n", current->pid, fd);
    if (copy_to_user(tw->outp, &fd, sizeof(fd))) {
        pr_err("install ksu fd reply err\n");
        ksu_close_fd(fd);
    }

    kfree(tw);
    module_put(THIS_MODULE);
}

static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs = PT_REAL_REGS(regs);
    int magic1 = (int)PT_REGS_PARM1(real_regs);
    int magic2 = (int)PT_REGS_PARM2(real_regs);
    unsigned int cmd = (unsigned int)PT_REGS_PARM3(real_regs);
    unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);
    unsigned long reply = (unsigned long)arg4;

    /* Check if this is a request to install KSU fd */
    if (magic1 == KSU_INSTALL_MAGIC1 && magic2 == KSU_INSTALL_MAGIC2) {
        struct ksu_install_fd_tw *tw;

        tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
        if (!tw)
            return 0;

        tw->outp = (int __user *)arg4;
        tw->cb.func = ksu_install_fd_tw_func;

        if (!try_module_get(THIS_MODULE)) {
            kfree(tw);
            return 0;
        }
        if (task_work_add(current, &tw->cb, TWA_RESUME)) {
            module_put(THIS_MODULE);
            kfree(tw);
            pr_warn("install fd add task_work failed\n");
        }
    }

    if (magic2 == CHANGE_MANAGER_UID) {
        /* only root is allowed for this command */
        if (current_uid().val != 0)
            return 0;

        pr_info("sys_reboot: ksu_set_manager_appid to: %d\n", cmd);
        ksu_set_manager_appid(cmd);

        if (cmd == ksu_get_manager_appid()) {
            if (copy_to_user((void __user *)arg4, &reply, sizeof(reply)))
                pr_info("sys_reboot: reply fail\n");
        }

        return 0;
    }

    if (magic2 == GET_SULOG_DUMP_V2) {
        if (current_uid().val != 0)
            return 0;

        int ret = ksu_sulog_handle_compat_dump((void __user *)arg4);
        if (ret)
            return 0;

        if (copy_to_user((void __user *)arg4, &reply, sizeof(reply) ))
            return 0;
    }

    if (magic2 == CHANGE_KSUVER) {
        if (current_uid().val != 0)
            return 0;

        pr_info("sys_reboot: ksu_change_ksuver to: %d\n", cmd);
        ksuver_override = cmd;

        if (copy_to_user((void __user *)arg4, &reply, sizeof(reply) ))
            return 0;
    }

    /* The legacy reboot-based uname mutation was intentionally retired.  It
     * was global state, allowed repeated changes, modified version as well as
     * release, and could target the host UTS.  Waydroid osrelease is now a
     * one-shot module parameter consumed only after Android PID 1 is found. */
    if (magic2 == CHANGE_SPOOF_UNAME) {
        pr_warn_ratelimited("sys_reboot: CHANGE_SPOOF_UNAME is retired\n");
        return 0;
    }

    return 0;
}

static struct kprobe reboot_kp = {
    .symbol_name = REBOOT_SYMBOL,
    .pre_handler = reboot_handler_pre,
};

void __init ksu_supercalls_init(void)
{
    int rc;

    ksu_supercall_dump_commands();

    rc = register_kprobe(&reboot_kp);
    if (rc) {
        pr_err("reboot kprobe failed: %d\n", rc);
    } else {
        pr_info("reboot kprobe registered successfully\n");
    }
}

void __exit ksu_supercalls_exit(void)
{
    unregister_kprobe(&reboot_kp);
    ksu_supercall_cleanup_state();
}
