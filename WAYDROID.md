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

## Supported prebuilt package

The GitHub release is the recommended installation path. It contains all three
matching artifacts: the Arch package (including `kernelsu.ko`, the patched
x86_64 `ksud`, loader and timer), the officially signed **spoofed Manager** APK
and a standalone copy of the module. The package is tied to the kernel release
shown in its release notes; check before installing:

```sh
uname -r
```

Download the package and Manager APK from the latest
[GitHub release](https://github.com/incapdns/KernelSU-Next-Waydroid/releases),
then install and configure the host package:

```sh
sudo pacman -U ./kernelsu-next-waydroid-*.pkg.tar.zst
sudo configure-waydroid-kernelsu
```

The configurator also registers a mount-namespace hook before Android `init`.
For the current Android 13 `TQ3A.230901.001` image it restores the literal AOSP
procfs policy `hidepid=2,gid=3009` and the AOSP VFS restrictions
`noatime,nosuid,nodev` on the container's `/data` bind mount. It does not alter
the host `/proc` or the host Btrfs mount. Verify the effective flags after a
container restart:

```bash
sudo kernelsu-waydroid-aosp-mount-audit
```

The complete source-to-runtime matrix, including every static AOSP r75 mount,
container substitutions and non-reproducible shared-kernel mounts, is in
[`docs/WAYDROID_AOSP_MOUNT_AUDIT.md`](docs/WAYDROID_AOSP_MOUNT_AUDIT.md).

`configure-waydroid-kernelsu` removes only the exact `reboot` entry from
Waydroid's seccomp deny list and preserves the original profile as
`waydroid.seccomp.pre-kernelsu`. Zygisk Next needs this syscall; the rest of the
Waydroid deny list remains unchanged.

Skip directly to **Start or restart Waydroid** after using the prebuilt package.

## Build from source

Building from source requires both the host kernel module and an Android x86_64
`ksud`. `makepkg` deliberately fails if either artifact is absent.

First build the module for the exact running kernel:

```sh
KERNEL_RELEASE=7.1.8-1-cachyos ./kernel/build-waydroid-cachyos.sh
```

The result is `kernel/out-waydroid/kernelsu.ko`. The module must match the
running kernel's release exactly. This checkout was first validated against
the installed CachyOS `7.1.8-1-cachyos` headers.

Then build `ksud` for Android x86_64. The upstream reproducible build uses
`cross` and requires a working Docker or Podman installation:

```sh
rustup update stable
cargo install cross --git https://github.com/cross-rs/cross --rev 66845c1
clang --target=aarch64-linux-gnu -c -nostdlib \
  -o userspace/ksud/.lkm_image_bootstrap.o \
  userspace/ksud/src/lkm_image_bootstrap.S
CROSS_NO_WARNINGS=0 cross build \
  --target x86_64-linux-android \
  --release \
  --manifest-path userspace/ksud/Cargo.toml
```

The required result is
`userspace/ksud/target/x86_64-linux-android/release/ksud`.

## Matching Manager

Install the official **spoofed** KernelSU Next `dev` Manager built by the
upstream CI for commit `234f6e040fcbca18b16d2398e1aa225712ec99ad`. The
matching artifact is
`KernelSU_Next_v3.3.0-25-g234f6e04-spoofed_33239-release.apk`; it contains the
x86_64 `ksud` from that same commit and uses UAPI version 2, exactly like the
kernel. Upstream changes its package ID to a randomized value while preserving
the official APK signing identity compiled into the module:

```text
certificate size:   998 (0x3e6)
certificate SHA256: 79e590113c4c4c0c222978e413a5faa801666957b1212a328e46c00c69821bf7
```

The spoofed and ordinary upstream APKs have been verified to share that exact
certificate. The spoofed APK is included in this repository's GitHub release
and avoids exposing the well-known `com.rifsxd.ksunext` package name. A Manager
built in an unrelated fork without the official signing secret has a different
certificate and will intentionally not be crowned by the kernel.

If the ordinary Manager is already installed, install the spoofed APK first,
open it and confirm that it reports **Rooted**, then remove the ordinary
`com.rifsxd.ksunext` app. Keeping both installed leaves the known package name
visible to application scanners.

## Package and install

```sh
cd packaging
makepkg -f
sudo pacman -U ./kernelsu-next-waydroid-*.pkg.tar.zst
sudo configure-waydroid-kernelsu
```

Install the matching Manager APK after starting the Waydroid session as shown
below.

## Start or restart Waydroid

After booting the matching host kernel:

```sh
waydroid session stop
sudo systemctl stop waydroid-container
sudo load-kernelsu --unload-first
sudo systemctl start waydroid-container
waydroid show-full-ui
```

On a fresh installation, wait until Android reports that user 0 is ready, then
install the matching Manager (skip this command when it is already installed):

```sh
waydroid app install ./KernelSU_Next_*-spoofed_*-release.apk
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
