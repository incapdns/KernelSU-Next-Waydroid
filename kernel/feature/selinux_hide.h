#ifndef __KSU_H_SELINUX_HIDE
#define __KSU_H_SELINUX_HIDE

#ifdef CONFIG_KSU_SELINUX
void ksu_selinux_hide_init(void);
void ksu_selinux_hide_exit(void);
void ksu_selinux_hide_drop_backup_if_unused(void);
void ksu_selinux_hide_handle_second_stage(void);
void ksu_selinux_hide_handle_post_fs_data(void);
#else
static inline void ksu_selinux_hide_init(void) { }
static inline void ksu_selinux_hide_exit(void) { }
static inline void ksu_selinux_hide_drop_backup_if_unused(void) { }
static inline void ksu_selinux_hide_handle_second_stage(void) { }
static inline void ksu_selinux_hide_handle_post_fs_data(void) { }
#endif

#endif
