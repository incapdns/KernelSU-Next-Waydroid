# KernelSU Next on Waydroid x86_64

This branch adapts KernelSU Next to the unusual case where Android runs in a
container and shares the Linux host kernel. It tracks the upstream `dev`
branch; an old commit is not configuration and does not need to be repeated
when upstream moves.

## What this project builds

The project produces two artifacts:

- `kernel/out-waydroid/kernelsu.ko`: an external module built against the
  headers of the exact host kernel release;
- `userspace/ksud/target/x86_64-linux-android/release/ksud`: the daemon that
  runs inside Android x86_64.

It does **not rebuild the full Linux kernel**, create a `boot.img`, or modify a
Waydroid image. The module builder invokes the installed kernel's Kbuild with
`CONFIG_KSU=m`, `CONFIG_KSU_NON_ANDROID=y`, Android SELinux integration
disabled, and the x86_64 syscall dispatcher enabled. Changing the host kernel
therefore requires rebuilding and reinstalling only `kernelsu.ko` against the
new headers.

This differs from the KeyMint-TEE project. That project modifies `vendor.img`
to inject the reference KeyMint service; it does not build a kernel.

## What the adaptation actually does

The following behavior was verified in the implementation, not inferred only
from this document:

- detects container PID 1 when it executes `/system/bin/init` and records its
  PID namespace;
- excludes every process in the host PID namespace from KernelSU's privileged
  syscall path;
- tracks Android init, Zygote, shell, Manager, and allowlisted applications;
- discovers Manager and reads the allowlist from a syscall context where
  `/data` refers to Android's `/data`;
- gives `ksud` an inheritable driver file descriptor before `exec`, avoiding
  the LXC seccomp restriction inherited by Android init;
- makes Android's in-kernel SELinux integration optional because the module is
  loaded into a distribution kernel;
- safely detaches global x86_64 dispatcher patches before module removal;
- resets KernelSU's one-shot boot hooks before each Waydroid init so that
  `post-fs-data` runs before Zygote;
- retains `ksud late-load` only as manual recovery for an already-running
  container.

The patched `ksud` also publishes a marker after its blocking late-load stages
finish and relaunches the actual Manager package, including randomized package
names.

## Why internal scripts remain

`waydroid-kernelsu` is the only public command. Files below
`/usr/lib/kernelsu-next-waydroid` are private implementations because different
actors execute them in different contexts:

- the `pre-start` hook runs on the host before the next Android init exists;
- the `mount` hook runs in the mount namespace prepared by LXC;
- late-load runs after boot and only for recovery;
- the auditor is read-only and examines an existing container namespace.

Merging those contexts into one large script would reduce the file count but
would mix distinct lifecycles and privilege boundaries. The public interface
is centralized while the necessary internal separation remains explicit.

## Prebuilt package

Check `uname -r` and select the package built for that exact kernel. The host
package and its compatible, officially signed Manager are published together
in the [latest release](https://github.com/incapdns/KernelSU-Next-Waydroid/releases/latest).

```sh
sudo pacman -U ./kernelsu-next-waydroid-*.pkg.tar.zst
sudo waydroid-kernelsu configure
waydroid-kernelsu status
```

Manager must retain the official signing identity accepted by the module. The
documentation does not duplicate a commit hash, versioned APK filename, or
certificate: compatible artifacts belong to the same release.

## Build and package

Build dependencies are the running kernel's headers, a module toolchain
(`make` and Clang), Rust/Cargo managed by `rustup`, and the Android NDK. The CLI
finds the packaged NDK automatically; Docker, Podman, and `cross` are not part
of the build path:

```sh
paru -S android-ndk
rustup default stable
rustup target add x86_64-linux-android
./waydroid-kernelsu package
sudo ./waydroid-kernelsu install
```

The NDK package exposes one stable path, `/opt/android-ndk`; its release number
is not duplicated in this repository. `ANDROID_NDK_HOME` remains available for
custom installations, and `KSU_ANDROID_API` can override the default API 26
compiler when required.

`package` rebuilds `kernelsu.ko`, rebuilds Android x86_64 `ksud`, and runs
`makepkg`. `install` selects only the newest matching output instead of passing
an ambiguous wildcard with old packages to pacman. `pkgver` is evaluated directly
from `git describe`; there is no manual version to synchronize. `pkgrel` remains
because it is the Arch packaging revision, not a second KernelSU version.

The module targets `uname -r` by default. To build for a different installed
kernel, set `KERNEL_RELEASE`; the script derives its header path. Set
`KERNEL_BUILD` only when those headers are not available at
`/usr/lib/modules/$KERNEL_RELEASE/build`.

## Runtime

After installing or upgrading the package:

```sh
sudo waydroid-kernelsu configure
waydroid session stop
sudo systemctl restart waydroid-container
waydroid show-full-ui
```

The configurator preserves backups of the seccomp profile and LXC config. It
removes only the exact `reboot` entry from the denylist and registers the mount
and pre-start hooks.

Available commands:

```text
waydroid-kernelsu build       # build the module and ksud
waydroid-kernelsu package     # build both and create the Arch package
waydroid-kernelsu install     # install the newest generated package
waydroid-kernelsu configure   # configure LXC and seccomp integration
waydroid-kernelsu status      # display effective state
waydroid-kernelsu audit       # audit the container mounts
waydroid-kernelsu reload      # safe module reload, with Waydroid stopped
waydroid-kernelsu recover     # manual late-load recovery
```

`reload` prepares the dispatcher for unload, removes the module, and loads it
again. Stop Waydroid first. Do not replace this sequence with `rmmod` or
`modprobe -r`: the module holds a self-reference until every global x86_64
entry point has been restored.

The mount hook currently reproduces audited AOSP behavior: `proc` uses
`hidepid=2,gid=3009`, `/data` uses `noatime,nosuid,nodev`, unwanted debugfs is
removed, and extra `/vendor` binds become read-only. It does not change host
mounts. The procfs and FUSE options remain present in the Android 17 source.
The original evidence for each decision is in
[`docs/WAYDROID_AOSP_MOUNT_AUDIT.md`](docs/WAYDROID_AOSP_MOUNT_AUDIT.md).

After Android starts, verify Zygisk Next with its own status command:

```sh
sudo waydroid shell -- /data/adb/modules/zygisksu/bin/zygiskd status
```

The expected result includes `inject_state:1`, successful states for both
Zygotes, KernelSU root, and no module with an issue.
