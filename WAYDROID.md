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
- reset KernelSU's one-shot Android boot hooks before each container init,
  allowing the real `post-fs-data` stage to finish before the first Zygote;
- retain `ksud late-load` as a manual recovery path, not as the normal boot
  mechanism.

## Adaptations removed because the upstream dispatcher replaces them

- the custom `non_android_syscall.c` stub;
- direct execution of setresuid/execve/stat/access handlers in `sys_enter`;
- separate `_non_android` sucompat handlers;
- `task_work` deferral for Manager FD installation, mount namespaces and
  kernel unmount. The upstream dispatcher invokes these handlers from normal
  syscall context, where sleeping operations are valid.

The dispatcher does not provide container isolation, `/data` discovery,
SELinux decoupling or Waydroid lifecycle integration; those adaptations remain
necessary.

## Supported prebuilt package

The GitHub release is the recommended installation path. It contains all three
matching artifacts: the Arch package (including `kernelsu.ko`, the patched
x86_64 `ksud`, loader and LXC hooks), the officially signed **spoofed Manager** APK
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

The configurator registers two LXC lifecycle hooks without changing
`waydroid-container.service`. The pre-start hook reloads KernelSU before every
Android init, resetting its one-shot init/Zygote hooks. The mount hook runs in
the new container namespace before Android `init`.
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

### Direct NDK build (without Docker/Podman)

The `cross` command above is the preferred upstream-compatible path and
provides its Android build environment itself. A direct Cargo build instead
requires Android NDK r27 (`27.0.12077973`) and the Rust Android target. The
following is the exact alternative used to build the packaged x86_64 daemon:

```sh
rustup target add x86_64-linux-android

# Set this to the installed NDK r27 directory. Avoid passing a path containing
# spaces to bindgen: expose it through a temporary path without spaces.
KSU_NDK_REAL="$ANDROID_SDK_ROOT/ndk/27.0.12077973"
KSU_NDK_LINK=/tmp/kernelsu-ndk-r27
test -e "$KSU_NDK_LINK" || ln -s "$KSU_NDK_REAL" "$KSU_NDK_LINK"

KSU_NDK_PREBUILT="$KSU_NDK_LINK/toolchains/llvm/prebuilt/linux-x86_64"
KSU_NDK_SYSROOT="$KSU_NDK_PREBUILT/sysroot"
export ANDROID_NDK_HOME="$KSU_NDK_LINK"
export ANDROID_NDK_ROOT="$KSU_NDK_LINK"
export CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER="$KSU_NDK_PREBUILT/bin/x86_64-linux-android35-clang"
export CC_x86_64_linux_android="$CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER"
export CXX_x86_64_linux_android="$KSU_NDK_PREBUILT/bin/x86_64-linux-android35-clang++"
export BINDGEN_EXTRA_CLANG_ARGS_x86_64_linux_android="--sysroot=$KSU_NDK_SYSROOT -I$KSU_NDK_SYSROOT/usr/include/x86_64-linux-android -I$KSU_NDK_SYSROOT/usr/include"
export LIBCLANG_PATH="$KSU_NDK_PREBUILT/musl/lib"

cargo build \
  --target x86_64-linux-android \
  --release \
  --manifest-path userspace/ksud/Cargo.toml
```

This is an alternative to `cross`, not an additional mandatory step. Users
installing a prebuilt package do not need Rust, Cargo, an NDK, Docker or
Podman; those tools are required only when rebuilding the package from source.

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
sudo systemctl start waydroid-container
waydroid show-full-ui
```

On a fresh installation, wait until Android reports that user 0 is ready, then
install the matching Manager (skip this command when it is already installed):

```sh
waydroid app install ./KernelSU_Next_*-spoofed_*-release.apk
```

The LXC pre-start hook loads KernelSU only when it is not already present. It
never unloads a live module during an ordinary Waydroid restart. Android init
then executes the genuine blocking `post-fs-data` action before
`app_process64` starts.

On x86_64 this build retains KernelSU Next's
`KSU_X86_PATCH_SYSCALL_DISPATCHER`. A raw `rmmod kernelsu` is intentionally
refused by a module self-reference: dynamically patched global syscall entry
points must be detached in a separate operation before `delete_module` can be
safe. To replace a loaded module, first stop Waydroid and use the packaged
loader:

```sh
waydroid session stop
sudo systemctl stop waydroid-container
sudo load-kernelsu --unload-first
```

The loader writes `1` to the root-only `prepare_unload` module parameter. The
kernel restores the original x86 dispatcher and every patched syscall-table
entry while the module is still pinned, returns to userspace, releases its
guard, and only then allows `rmmod`. Module teardown can remove the Android
daemon, so the loader restores the package-matched `ksud` into Waydroid's
`data/adb` before reloading the module. Never bypass this sequence with
`rmmod`, `modprobe -r`, or `load-kernelsu --unload-first` while Waydroid is
running.

The late-load helper and unit remain installed for manual recovery of an
already-running container. The periodic timer is intentionally inactive:
late-load occurs after Zygote and therefore cannot replace the normal
pre-Zygote `post-fs-data` contract required by Zygisk modules.

### Verification

After Android reports that user 0 is ready, verify Zygisk Next with its own
status command:

```sh
sudo waydroid shell -- \
  /data/adb/modules/zygisksu/bin/zygiskd status
```

Require `inject_state:1`, successful states for both Zygotes,
`root_status:✅KernelSU`, and `modules_with_issue:0`. Process-name searches are
not authoritative because the daemon need not retain a public `zygisk` marker.
