# Waydroid mount audit against AOSP Android 13

## Authority and scope

The running image reports this build fingerprint:

```
waydroid/waydroid_x86_64/waydroid_x86_64:13/TQ3A.230901.001/eng.aleast.20260403.132616:user/release-keys
```

`TQ3A.230901.001` is AOSP tag `android-13.0.0_r75`. This audit uses the
following files from that exact tag:

- `platform/system/core/init/first_stage_init.cpp`
- `platform/system/core/init/init.cpp`
- `platform/system/core/rootdir/init.rc`
- `platform/system/core/rootdir/init-debug.rc`
- `device/generic/goldfish/init.ranchu.rc`
- `device/generic/goldfish/fstab.ranchu.x86`

The observed side is `/proc/self/mountinfo` read in the mount and PID namespace
of the running Waydroid init. `/proc/mounts` alone is not sufficient because it
does not expose mount propagation or the bind source root.

This document distinguishes an exact mount from a container substitution. A
substitution is never called exact merely because it looks similar.

## Literal first-stage mounts

| Target | Literal AOSP r75 request | Effective Waydroid state | Result |
|---|---|---|---|
| `/dev` | `tmpfs`, `MS_NOSUID`, `mode=0755` | `tmpfs`, `rw,nosuid`, `mode=0755`; kernel-added `relatime,inode64,huge=advise` | Exact explicit AOSP flags; extra values are kernel serialization/defaults on the host kernel |
| `/dev/pts` | `devpts`, flags `0`, data `NULL` | `devpts`, `rw,nosuid,noexec`, `gid=5,mode=620,ptmxmode=666,max=10` | Not literal; LXC owns the PTY allocation and imposes its isolation options |
| `/proc` | `proc`, flags `0`, `hidepid=2,gid=3009` | Before fix: no `hidepid` or `gid`; after hook: `rw,nosuid,nodev,noexec`, `gid=3009,hidepid=invisible` | Corrected. Linux serializes numeric `hidepid=2` as `hidepid=invisible` |
| `/sys` | `sysfs`, flags `0`, data `NULL` | `sysfs`, `ro` | Not literal. Making the shared host sysfs writable would grant Android writes against the host kernel and hardware; there is no mount-namespace-only equivalent for a global sysfs instance |
| `/sys/fs/selinux` | `selinuxfs`, flags `0`, data `NULL` | Absent because SELinux is disabled in the host kernel command line/runtime | Not reproducible while SELinux is disabled; a fake mount would not be equivalent |
| `/mnt` | `tmpfs`, `MS_NOEXEC|MS_NOSUID|MS_NODEV`, `mode=0755,uid=0,gid=1000` | Same filesystem, flags, mode, UID and GID | Exact |
| `/debug_ramdisk` | transient `tmpfs`, `MS_NOEXEC|MS_NOSUID|MS_NODEV`, `mode=0755,uid=0,gid=0` | Absent after second-stage init | Expected final state: AOSP unmounts it during second stage |
| `/second_stage_resources` | transient `tmpfs`, same flags and ownership as `/debug_ramdisk` | Absent after second-stage init | Expected final state: AOSP unmounts it during second stage |

The AOSP first stage also changes `/proc/cmdline` and `/proc/bootconfig` to
mode `0440`. Those are permissions, not mount operations, and are tracked
separately from this mount table.

## Literal second-stage and root `init.rc` mounts

| Target | Literal AOSP r75 request | Effective Waydroid state | Result |
|---|---|---|---|
| `/apex` | `tmpfs`, `MS_NOEXEC|MS_NOSUID|MS_NODEV`, `mode=0755,uid=0,gid=0` | Same | Exact |
| `/linkerconfig` | same as `/apex` | Same | Exact |
| `/linkerconfig` bootstrap bind | bind-recursive `/linkerconfig/bootstrap` over `/linkerconfig` | Mount root is `/bootstrap` from the same tmpfs | Exact |
| `/sys/kernel/tracing` | `tracefs`, `gid=3012` | `tracefs`, `rw,gid=3012` | Exact |
| `/sys/kernel/debug` | conditional `debugfs`; unmounted at boot completion unless explicitly retained | Mounted `debugfs,rw`; `ro.debuggable=0` and no AOSP debugfs-restrictions property are set, while Waydroid explicitly keeps it for WebView tracing | Deliberate Waydroid divergence, not the final state selected by this image's AOSP properties |
| `/config` | `configfs`, `nodev,noexec,nosuid` | Same | Exact |
| `/dev/binderfs` | `binder`, `stats=global`, followed by binder device symlinks | No `/dev/binderfs`; three binder devices are host binderfs bind mounts at `/dev/{binder,hwbinder,vndbinder}` | Container substitution, not literal; required by Waydroid's host binder architecture |
| `/sys/fs/fuse/connections` | `fusectl`, no explicit flags/data | Same | Exact explicit AOSP request |
| `/sys/fs/bpf` | `bpf`, `nodev,noexec,nosuid` | Same | Exact |
| `/sys/fs/pstore` | `pstore`, `nodev,noexec,nosuid` | Same | Exact |
| `/` post-fs | bind-remount `ro,nodev` | overlay root `ro,nodev` | Exact final VFS flags; backing filesystem is a Waydroid substitution |
| `/storage` | recursive bind of `/mnt/user/0`, then recursive slave propagation | Mount root `/user/0` from `/mnt` tmpfs with slave relationship to `/mnt` | Exact |
| `/data/user/0` | recursive bind of `/data/data` | Bind root is host-backed Waydroid `/data/data` | Exact bind relationship; backing filesystem is a substitution |
| `/data_mirror` | `tmpfs`, `nodev,noexec,nosuid`, `mode=0700,uid=0,gid=1000` | Same | Exact |
| `/data_mirror/data_ce/null` | recursive bind of `/data/user` | Same logical source, backed by host Btrfs | Exact bind relationship; backing filesystem is a substitution |
| `/data_mirror/data_de/null` | recursive bind of `/data/user_de` | Same logical source, backed by host Btrfs | Exact bind relationship; backing filesystem is a substitution |
| `/data_mirror/misc_ce/null` | recursive bind of `/data/misc_ce` | Same logical source, backed by host Btrfs | Exact bind relationship; backing filesystem is a substitution |
| `/data_mirror/misc_de/null` | recursive bind of `/data/misc_de` | Same logical source, backed by host Btrfs | Exact bind relationship; backing filesystem is a substitution |
| `/data_mirror/cur_profiles` | recursive bind of `/data/misc/profiles/cur` | Same | Exact bind relationship |
| `/data_mirror/ref_profiles` | recursive bind of `/data/misc/profiles/ref` | Same | Exact bind relationship |

APEX package mounts are dynamic outputs of `apexd`, not hard-coded mount
requests in `first_stage_init.cpp`. The running APEX mounts are read-only, as
expected. Their backing source is the Waydroid system overlay instead of an
Android block device.

## x86_64 emulator fstab versus Waydroid storage substitution

The exact `device/generic/goldfish/fstab.ranchu.x86` entries for r75 are:

| Target | Filesystem and literal mount flags | Waydroid |
|---|---|---|
| `/system` | ext4, `ro,barrier=1` | Root/system overlay supplied by host |
| `/vendor` | ext4, `ro,barrier=1` | loop ext4 plus a read-only overlay; host-provided `waydroid.prop` and `host-permissions` binds are remounted `ro` in the container so they do not create writable holes |
| `/product` | ext4, `ro,barrier=1` | Part of the system overlay |
| `/system_dlkm` | erofs, `ro` | Absent as an independent mount |
| `/system_ext` | ext4, `ro,barrier=1` | Part of the system overlay |
| `/data` | ext4, `noatime,nosuid,nodev,nomblk_io_submit,errors=panic` | Host Btrfs bind mount with the equivalent VFS flags `noatime,nosuid,nodev`; ext4-only flags cannot exist on Btrfs |
| `/metadata` | ext4, `noatime,nosuid,nodev` | Absent |
| removable storage | `auto`, defaults, vold-managed | Waydroid FUSE/pass-through storage |
| swap | zram, defaults, 75% sizing | Host-controlled memory and swap |

These are not fixable by changing flags alone. Reproducing them literally
would require assigning Android its own block devices, dm-linear mappings,
AVB chain, metadata partition, encryption keys and zram. Waydroid deliberately
sets `mount_all /dev/null` and supplies system/vendor/data from the host.

The VFS flag mismatch on `/data` is corrected by the LXC mount hook using a
bind remount inside the container namespace. `nosuid` disables set-ID bits; it
does not apply `noexec`, so KernelSU can still execute module scripts below
`/data/adb`. `nodev` prevents device-node interpretation, matching AOSP.

## Container-only mounts

The following have no literal AOSP first-stage counterpart and therefore must
not be mislabeled as AOSP mounts: `/mnt_extra`, `/tmp`, `/var`, `/run`, host
Wayland and Pulse sockets, host device bind mounts, `/sys/fs/cgroup`,
`/vendor/waydroid.prop`, `/vendor/etc/host-permissions`, translation
`binfmt_misc`, and host pass-through storage binds.

They require a separate least-privilege Waydroid audit. Removing them merely
to make the list shorter would not replicate AOSP and would break container
functionality.

## Implemented correction

`/usr/lib/kernelsu-next-waydroid/aosp-mount-hook` is registered as an
`lxc.hook.mount`. LXC documents that this hook executes inside the container's
mount namespace after automatic mounts and before `pivot_root`. It remounts
only the container procfs with the literal Android 13 data options and fails
container startup if the effective mount does not report them.

Run the verifier after each Waydroid update:

```bash
sudo kernelsu-waydroid-aosp-mount-audit
```

The correction is intentionally attached to the generated LXC configuration,
not the upstream `waydroid-container.service`.
