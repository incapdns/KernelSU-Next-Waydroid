#ifndef __KSU_SUSFS_BRIDGE_H
#define __KSU_SUSFS_BRIDGE_H

#include <linux/errno.h>
#include <linux/types.h>

#ifdef CONFIG_KSU_SUSFS
int ksu_susfs_bridge_init(void);
void ksu_susfs_bridge_exit(void);
void ksu_susfs_bridge_post_fs_data(void);
void ksu_susfs_bridge_waydroid_exit(void);
void ksu_susfs_bridge_mark_app(uid_t old_uid, uid_t new_uid);
void ksu_susfs_bridge_sync_selinux(void);
long ksu_susfs_bridge_dispatch(unsigned int cmd, unsigned long arg);
#else
static inline int ksu_susfs_bridge_init(void) { return 0; }
static inline void ksu_susfs_bridge_exit(void) { }
static inline void ksu_susfs_bridge_post_fs_data(void) { }
static inline void ksu_susfs_bridge_waydroid_exit(void) { }
static inline void ksu_susfs_bridge_mark_app(uid_t old_uid, uid_t new_uid) { }
static inline void ksu_susfs_bridge_sync_selinux(void) { }
static inline long ksu_susfs_bridge_dispatch(unsigned int cmd,
					      unsigned long arg)
{
	return -EOPNOTSUPP;
}
#endif

#endif
