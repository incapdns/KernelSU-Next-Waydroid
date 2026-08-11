# KernelSU Next `dev` for Waydroid on CachyOS

This branch starts from KernelSU Next `dev` commit
`234f6e040fcbca18b16d2398e1aa225712ec99ad` and keeps the upstream x86_64
syscall dispatcher. It does not carry the old direct-syscall tracepoint fork.

## Waydroid adaptations retained

- detect container PID 1 executing `/system/bin/init`;
- exclude every process in the host PID namespace;
- mark Android init/zygote, shell and allowlisted applications;
- discover the signed Manager while running in Waydroid's `/data/app` view;
- load `/data/adb/ksu/.allowlist` from an Android syscall context;
- make Android-internal SELinux integration optional for a distribution host;
- use the caller's Android PID/mount namespace for namespace operations;
- keep the LKM visible and unloadable;
- unregister lifecycle/syscall tracepoints before dispatcher teardown;
- run `ksud late-load` once per Waydroid container instance through a systemd
  integration installed by the package.

## Adaptations removed because the upstream dispatcher replaces them

- the custom `non_android_syscall.c` stub;
- direct execution of setresuid/execve/stat/access handlers in `sys_enter`;
- separate `_non_android` sucompat handlers;
- `task_work` deferral for Manager FD installation, mount namespaces and
  kernel unmount. The upstream dispatcher invokes these handlers from normal
  syscall context, where sleeping operations are valid.

The dispatcher does not provide container isolation, `/data` discovery,
SELinux decoupling or `ksud late-load`; those adaptations remain necessary.

## Build

```sh
KERNEL_RELEASE=7.1.8-1-cachyos ./kernel/build-waydroid-cachyos.sh
```

The result is `kernel/out-waydroid/kernelsu.ko`. The module must match the
running kernel's release exactly. This checkout was first validated against
the installed CachyOS `7.1.8-1-cachyos` headers.

## Package and install

```sh
cd packaging
makepkg -f
sudo pacman -U ./kernelsu-next-waydroid-*.pkg.tar.zst
```

After booting the matching kernel:

```sh
sudo load-kernelsu --unload-first
waydroid session stop
sudo systemctl restart waydroid-container
waydroid show-full-ui
```

The systemd drop-in restarts the late-load helper whenever the Waydroid
container starts. It waits for Android boot completion and `/data/adb/ksud`,
then runs `ksud late-load` once for that container init instance.
