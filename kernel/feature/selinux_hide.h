#ifndef __KSU_H_SELINUX_HIDE
#define __KSU_H_SELINUX_HIDE

#include <linux/types.h>

#ifdef CONFIG_KSU_SELINUX
void ksu_selinux_hide_init(void);
void ksu_selinux_hide_exit(void);
void ksu_selinux_hide_drop_backup_if_unused(void);
void ksu_selinux_hide_handle_second_stage(void);
void ksu_selinux_hide_handle_post_fs_data(void);
bool ksu_selinux_hide_is_enabled(void);
bool ksu_selinux_hide_is_running(void);
#else
static inline void ksu_selinux_hide_init(void) { }
static inline void ksu_selinux_hide_exit(void) { }
static inline void ksu_selinux_hide_drop_backup_if_unused(void) { }
static inline void ksu_selinux_hide_handle_second_stage(void) { }
static inline void ksu_selinux_hide_handle_post_fs_data(void) { }
static inline bool ksu_selinux_hide_is_enabled(void) { return false; }
static inline bool ksu_selinux_hide_is_running(void) { return false; }
#endif

#endif
