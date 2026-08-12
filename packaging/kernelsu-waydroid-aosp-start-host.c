#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int log_fd = STDERR_FILENO;

static int fail(const char *operation)
{
    dprintf(log_fd, "kernelsu-waydroid-aosp-start-host: %s: %s\n",
            operation, strerror(errno));
    return EXIT_FAILURE;
}

static bool parse_pid(const char *text, pid_t *pid)
{
    char *end = NULL;
    long value;

    if (!text || !*text)
        return false;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end || value <= 0)
        return false;
    *pid = (pid_t)value;
    return (long)*pid == value;
}

int main(void)
{
    const char *pid_text = getenv("LXC_PID");
    const char *rootfs = getenv("LXC_ROOTFS_MOUNT");
    char namespace_path[64], root_path[64];
    struct stat st;
    pid_t pid;
    int namespace_fd, root_fd;

    {
        int fd = open("/run/kernelsu-waydroid-aosp-start-host.log",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0)
            log_fd = fd;
    }
    dprintf(log_fd, "--- start-host pid=%s rootfs=%s ---\n",
            pid_text ? pid_text : "(null)", rootfs ? rootfs : "(null)");

    if (!parse_pid(pid_text, &pid)) {
        dprintf(log_fd, "kernelsu-waydroid-aosp-start-host: invalid LXC_PID\n");
        return EXIT_FAILURE;
    }
    if (!rootfs || !*rootfs) {
        dprintf(log_fd,
                "kernelsu-waydroid-aosp-start-host: LXC_ROOTFS_MOUNT is unset\n");
        return EXIT_FAILURE;
    }

    if (snprintf(root_path, sizeof(root_path), "/proc/%ld/root", (long)pid) >=
        (int)sizeof(root_path)) {
        errno = ENAMETOOLONG;
        return fail("format container root path");
    }
    root_fd = open(root_path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0)
        return fail("open container process root");

    if (snprintf(namespace_path, sizeof(namespace_path), "/proc/%ld/ns/mnt",
                 (long)pid) >= (int)sizeof(namespace_path)) {
        errno = ENAMETOOLONG;
        return fail("format mount namespace path");
    }
    namespace_fd = open(namespace_path, O_RDONLY | O_CLOEXEC);
    if (namespace_fd < 0)
        return fail("open container mount namespace");
    if (setns(namespace_fd, CLONE_NEWNS) < 0)
        return fail("setns mount namespace");
    close(namespace_fd);

    /* root_fd was opened through the child's process root before setns, so it
     * refers to the root mount in the child's namespace rather than LXC's
     * host-side staging directory. No container program/linker is executed. */
    if (fchdir(root_fd) < 0)
        return fail("fchdir container rootfs");
    if (chroot(".") < 0)
        return fail("chroot container rootfs");
    if (chdir("/") < 0)
        return fail("chdir container root");
    close(root_fd);

    /* Android 13 r75 first_stage_init.cpp uses flags=0 and data=NULL. LXC's
     * generated config instead adds nosuid,noexec,gid=5,mode=620,ptmxmode=666
     * and max=10, then bind-mounts devpts/ptmx. Remove both substitutions. */
    if (umount2("/dev/ptmx", 0) < 0 && errno != EINVAL && errno != ENOENT)
        return fail("unmount LXC /dev/ptmx bind");
    if (umount2("/dev/pts", 0) < 0) {
        if (errno != EBUSY || umount2("/dev/pts", MNT_DETACH) < 0)
            return fail("detach LXC devpts");
    }
    if (mount("devpts", "/dev/pts", "devpts", 0, NULL) < 0)
        return fail("mount AOSP devpts");

    if (unlink("/dev/ptmx") < 0 && errno != ENOENT)
        return fail("remove LXC ptmx inode");
    if (mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2)) < 0)
        return fail("create AOSP /dev/ptmx");
    if (chown("/dev/ptmx", 0, 0) < 0)
        return fail("chown /dev/ptmx");
    if (chmod("/dev/ptmx", 0666) < 0)
        return fail("chmod /dev/ptmx");
    if (stat("/dev/ptmx", &st) < 0)
        return fail("stat /dev/ptmx");
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != 5 ||
        minor(st.st_rdev) != 2 || (st.st_mode & 07777) != 0666 ||
        st.st_uid != 0 || st.st_gid != 0) {
        dprintf(log_fd,
                "kernelsu-waydroid-aosp-start-host: unexpected /dev/ptmx state\n");
        return EXIT_FAILURE;
    }

    dprintf(log_fd, "kernelsu-waydroid-aosp-start-host: success\n");
    return EXIT_SUCCESS;
}
