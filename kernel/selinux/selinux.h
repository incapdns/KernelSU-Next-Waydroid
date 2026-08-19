#ifndef __KSU_H_SELINUX
#define __KSU_H_SELINUX

#include <linux/types.h>
#include <linux/version.h>
#include <linux/cred.h>

#define KERNEL_SU_DOMAIN "ksu"
#define KERNEL_SU_FILE "ksu_file"

#define KERNEL_SU_CONTEXT "u:r:" KERNEL_SU_DOMAIN ":s0"
#define KSU_FILE_CONTEXT "u:object_r:" KERNEL_SU_FILE ":s0"
#define ZYGOTE_CONTEXT "u:r:zygote:s0"
#define INIT_CONTEXT "u:r:init:s0"
#define PRIV_APP_CONTEXT "u:r:priv_app:s0:c512,c768"

#ifdef CONFIG_KSU_SELINUX
void setup_selinux(const char *, struct cred *);

void setenforce(bool);

bool getenforce();

void cache_sid(void);

bool is_task_ksu_domain(const struct cred *cred);

bool is_ksu_domain();

bool is_zygote(const struct cred *cred);

bool is_init(const struct cred *cred);

u32 ksu_get_cached_su_sid(void);
u32 ksu_get_cached_zygote_sid(void);
u32 ksu_get_cached_priv_app_sid(void);

void apply_kernelsu_rules();

int handle_sepolicy(void __user *user_data, u64 data_len);

void setup_ksu_cred();

void escape_to_root_for_adb_root();

extern u32 ksu_file_sid;
#else
#include <linux/errno.h>

static inline void setup_selinux(const char *domain, struct cred *cred) { }
static inline void setenforce(bool enforce) { }
static inline bool getenforce(void) { return false; }
static inline void cache_sid(void) { }
static inline bool is_task_ksu_domain(const struct cred *cred) { return true; }
static inline bool is_ksu_domain(void) { return true; }
static inline bool is_zygote(const struct cred *cred) { return false; }
static inline bool is_init(const struct cred *cred) { return false; }
static inline u32 ksu_get_cached_su_sid(void) { return 0; }
static inline u32 ksu_get_cached_zygote_sid(void) { return 0; }
static inline u32 ksu_get_cached_priv_app_sid(void) { return 0; }
static inline void apply_kernelsu_rules(void) { }
static inline int handle_sepolicy(void __user *data, u64 len) { return -EOPNOTSUPP; }
static inline void setup_ksu_cred(void) { }
static inline void escape_to_root_for_adb_root(void) { }
#define ksu_file_sid 0U
#endif

#endif
