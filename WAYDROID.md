# KernelSU Next on Waydroid x86_64

This branch adapts KernelSU Next to the unusual case where Android runs in a
container and shares the Linux host kernel. It tracks the upstream `dev`
branch; an old commit is not configuration and does not need to be repeated
when upstream moves.

## What this project builds

The project produces two coordinated release artifacts:

- `kernel/out-waydroid/kernelsu.ko`: an external module built against the
  headers of the exact host kernel release;
- the Manager APK, which embeds ABI-matched `ksud` binaries for both
  `arm64-v8a` and `x86_64`.

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
- virtualizes `/proc/modules`, OverlayFS `statfs`, and the build identity in
  `/proc/version` only for non-privileged Waydroid readers while preserving
  the host/root view;
- safely detaches global x86_64 dispatcher patches before module removal;
- resets KernelSU's one-shot boot hooks before each Waydroid init so that
  `post-fs-data` runs before Zygote;
- obtains `ksud` only from the signed Manager APK, avoiding a second host copy
  that can drift from or race the Manager's embedded binary.

Ordinary Android processes retain the LXC `reboot(2)` seccomp denial. KernelSU
boot integration does not require globally exposing its reboot-magic transport.

## Why internal scripts remain

`waydroid-kernelsu` is the only public command. Files below
`/usr/lib/kernelsu-next-waydroid` are private implementations because different
actors execute them in different contexts:

- the `pre-start` hook runs on the host before the next Android init exists;
- the `mount` hook runs in the mount namespace prepared by LXC;
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

Host-package build dependencies are the running kernel's headers and a module
toolchain (`make` and Clang). Manager release CI separately builds both Android
`ksud` ABIs and embeds them before Gradle packages the APK:

```sh
./waydroid-kernelsu package
sudo ./waydroid-kernelsu install
```

`package` rebuilds `kernelsu.ko` and runs `makepkg`. `install` selects only the
newest matching output instead of passing
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
ensures that the exact `reboot` entry remains in the denylist and registers the
mount and lifecycle hooks.

The host hooks serialize the external module lifecycle through
`/run/kernelsu-next-waydroid/lifecycle.state`. `post-stop` only publishes
`stop-pending` and queues `kernelsu-waydroid-unload.service`; it never unloads
module text from inside the `lxc-start` teardown stack. The reconciler requires
LXC to report `STOPPED`, requires every Waydroid `lxc-start`/`lxc-stop` process
to have exited, waits one additional second, and only then requests safe
unload. A systemd timer retries pending work, including after the hook or
worker is killed. `pre-start` accepts only `unloaded` (or the initial state
after boot), so a new start remains refused until teardown really completes.

The x86 syscall wrappers pin `kernelsu.ko` while a raw syscall-table entry has
a return address in module text. `prepare_unload` restores every global entry
point first and refuses to release the final guard while such a frame is still
active. The same preparation unregisters the `/proc/version` return probe,
restores the `/proc/modules` and OverlayFS callbacks, and waits for their
per-call references and counters to drain. This prevents the observed `read`
return use-after-free.

Available commands:

```text
waydroid-kernelsu build       # build the module and ksud
waydroid-kernelsu package     # build both and create the Arch package
waydroid-kernelsu install     # install the newest generated package
waydroid-kernelsu configure   # configure LXC and seccomp integration
waydroid-kernelsu status      # display effective state
waydroid-kernelsu audit       # audit the container mounts
waydroid-kernelsu reload      # safe module reload, with Waydroid stopped
waydroid-kernelsu uts         # manage the container hostname/domainname
```

## Isolated UTS identity

Waydroid already starts in a UTS namespace separate from the host. UTS is a
kernel namespace, not a filesystem such as FUSE: each namespace owns the
values returned as hostname and NIS domainname. The host keeps its own values,
and every process created inside Waydroid observes the container values.

The userspace layer intentionally changes only `hostname` and `domainname`.
An optional KernelSU layer can additionally replace the `release` + `version`
pair exactly once and atomically in the Waydroid UTS. It never changes
`machine`, and it refuses the initial host UTS namespace. The host-side loader reads
`/proc/sys/kernel/osrelease` before the container starts, rather than trusting
`uname -r`, so a SUSFS read-time uname spoof cannot select the wrong module.

Enable a persistent identity and inspect the configuration with:

```sh
sudo waydroid-kernelsu uts enable android-device localdomain
sudo waydroid-kernelsu uts kernel enable 6.12.0-android16-0-gki '#1 SMP PREEMPT Tue Jan 2 03:04:05 UTC 2024'
waydroid-kernelsu uts show
waydroid session stop
sudo systemctl restart waydroid-container
waydroid show-full-ui
sudo waydroid-kernelsu uts show
sudo waydroid-kernelsu audit
```

The first `show` reports the desired values. The second, after the container
restart, also reads the effective values from the running container's UTS
namespace. `audit` fails if the configured and effective identities differ.
No command restarts Waydroid implicitly.

At startup, LXC creates the isolated namespaces and invokes the mount hook in
the container namespace before `pivot_root` and Android init. The hook writes
the configured values through the container procfs files
`/proc/sys/kernel/hostname` and `/proc/sys/kernel/domainname`, then reads them
back for verification. Because LXC protects `/proc/sys` with a read-only bind,
the hook makes only that container-side bind writable for these two writes and
restores it to read-only before Android init can run. The persistent source is:

```text
/etc/kernelsu-next-waydroid/uts-identity.conf
```

Use the public command instead of editing that file by hand; it validates DNS
label syntax and both kernel-identity fields, then writes the configuration
atomically.

The kernel-identity path is kernel-side. Before LXC creates Android PID 1, the
host pre-start hook loads `kernelsu.ko` and stages both configured values in
the root-only `waydroid_osrelease` and `waydroid_version` module parameters.
The existing exec tracepoint
then recognizes `/system/bin/init`, rejects `init_uts_ns`, retains a reference
to the container UTS, saves its original pair under `uts_sem`, and performs
one atomic replacement. Parameter writes return `EBUSY` after consumption. Android
init exit restores the original pair and releases the namespace reference;
module teardown repeats that restoration idempotently as a safety fallback.
The retired reboot supercall can no longer mutate `release` or `version`.

BRENE does not contain a genuine Pixel kernel identity in its PIF profile;
those profiles contain Android build and attestation properties only. Its
optional **Sync with Waydroid UTS** control therefore publishes the effective
`uname -r`/`uname -v` pair to `/data/adb/brene/waydroid-uts.conf`. The host
pre-start hook discovers the Waydroid data bind, rejects symlinks or unsafe
ownership/mode, validates the request, and synchronizes the persistent host
configuration for the next init. Android is never given write access to host
`/etc`.

Disable both overrides and return to the real/default identity on the next
restart with:

```sh
sudo waydroid-kernelsu uts disable
sudo waydroid-kernelsu uts kernel disable
waydroid session stop
sudo systemctl restart waydroid-container
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
