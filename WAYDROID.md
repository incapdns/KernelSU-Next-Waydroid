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

## Matching Manager

Install the official, non-spoofed KernelSU Next `dev` Manager built by the
upstream CI for commit `234f6e040fcbca18b16d2398e1aa225712ec99ad` before
testing this kernel. The matching artifact is
`KernelSU_Next_v3.3.0-25-g234f6e04_33239-release.apk`; it contains the x86_64
`ksud` from that same commit and uses UAPI version 2, exactly like the kernel.
Its APK v2 certificate is the identity compiled into the module:

```text
certificate size:   998 (0x3e6)
certificate SHA256: 79e590113c4c4c0c222978e413a5faa801666957b1212a328e46c00c69821bf7
```

The verified APK is included in this repository's GitHub release. A Manager
built in an unrelated fork without the official signing secret has a different
certificate and will intentionally not be crowned by the kernel. A custom
Manager remains possible, but requires compiling the kernel with that custom
APK certificate's size and SHA-256.

## Package and install

```sh
cd packaging
makepkg -f
sudo pacman -U ./kernelsu-next-waydroid-*.pkg.tar.zst
```

After booting the matching kernel:

```sh
waydroid session stop
sudo systemctl stop waydroid-container
sudo load-kernelsu --unload-first
sudo systemctl start waydroid-container
waydroid show-full-ui
```

Always stop both the Android session and container before unloading the kernel
module. `load-kernelsu --unload-first` restores the package-matched `ksud` into
Waydroid's `data/adb` after module teardown. This is required because Android
init must find `/data/adb/ksud` during its real `post-fs-data` stage, before the
first Zygote; restoring it later through `late-load` is too late for Zygisk.

The packaged systemd timer retries the late-load helper while Waydroid becomes
ready. It handles module updates and the remaining late-load stages once per
container init instance. It does not replace the pre-Zygote `post-fs-data`
stage described above and does not modify Waydroid's own service unit.

### Verification

After Android reports that user 0 is ready, verify Zygisk Next with:

```sh
sudo waydroid shell -- sh -c \
  'ps -A | grep -E "zn-daemon|zn-zygisk-companion"'
```

At least `zn-daemon` must be present. A Zygisk module such as Integrity Box
also creates a matching `zn-zygisk-companion` process after injection.
