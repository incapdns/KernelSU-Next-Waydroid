#ifndef __KSU_H_HOOK_MANAGER
#define __KSU_H_HOOK_MANAGER

#include <asm/ptrace.h>
#include <linux/sched.h>

// Hook manager initialization and cleanup
void ksu_syscall_hook_manager_init(void);
void ksu_syscall_hook_manager_exit(void);

/* True only for tasks in the PID namespace whose PID 1 executed Android init. */
bool ksu_is_waydroid_task(struct task_struct *task);

// extras.c
void __init ksu_avc_spoof_init(void);
void __exit ksu_avc_spoof_exit(void);

#endif
