#!/usr/bin/env bash
set -euo pipefail

osrelease_file=${KSU_KERNEL_OSRELEASE_FILE:-/proc/sys/kernel/osrelease}
kernel_release=${KERNEL_RELEASE:-$(<"$osrelease_file")}
if [[ -z $kernel_release || $kernel_release == */* ]]; then
    echo "Invalid kernel release from ${osrelease_file}." >&2
    exit 1
fi
kernel_build=${KERNEL_BUILD:-/usr/lib/modules/${kernel_release}/build}
repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
if [[ -z ${KERNEL_BUILD:-} && \
      ! -f ${kernel_build}/source/security/selinux/include/objsec.h ]]; then
    workspace_root=$(realpath "$repo_root/../..")
    for candidate in "$workspace_root"/components/cachyos-kernel-*-source/*/out-susfs; do
        if [[ -r $candidate/include/config/kernel.release && \
              $(<"$candidate/include/config/kernel.release") == "$kernel_release" && \
              -f $candidate/source/security/selinux/include/objsec.h ]]; then
            kernel_build=$candidate
        fi
    done
fi
output_dir=${OUTPUT_DIR:-${repo_root}/kernel/out-waydroid}
ksu_git_version=$(git -C "$repo_root" rev-list --count HEAD)
ksu_git_tag=$(git -C "$repo_root" describe --tags --abbrev=0 2>/dev/null || \
    git -C "$repo_root" rev-parse --short HEAD)
build_root=$(mktemp -d /tmp/kernelsu-next-waydroid.XXXXXX)
source_dir=${build_root}/kernel

cleanup() {
    rm -rf -- "$build_root"
}
trap cleanup EXIT

if [[ ! -f ${kernel_build}/Makefile ]]; then
    echo "Kernel build directory not found: ${kernel_build}" >&2
    exit 1
fi
if [[ ! -f ${kernel_build}/source/include/linux/susfs_ksu.h &&
      ! -f ${kernel_build}/include/linux/susfs_ksu.h ]]; then
    echo "Kernel build does not expose the CachyOS SUSFS bridge: ${kernel_build}" >&2
    exit 1
fi

# External-module paths containing spaces are split by Kbuild, so compile a
# temporary source copy and return only the resulting module.
mkdir -p "$source_dir" "$output_dir"
cp -a "${repo_root}/kernel/." "$source_dir/"
cp -a "${repo_root}/uapi" "${build_root}/uapi"

make -C "$kernel_build" \
    M="$source_dir" \
    src="$source_dir" \
    modules \
    CONFIG_KSU=m \
    CONFIG_KSU_SUSFS=y \
    CONFIG_KSU_NON_ANDROID=y \
    CONFIG_KSU_SELINUX=y \
    CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y \
    KSU_GIT_VERSION="$ksu_git_version" \
    KSU_GIT_TAG="$ksu_git_tag" \
    KSU_GIT_VERSION_VALID=1 \
    CC=clang \
    LLVM=1 \
    LLVM_IAS=1 \
    KBUILD_MODPOST_WARN=1 \
    -j"$(nproc)"

cp -f "${source_dir}/kernelsu.ko" "${output_dir}/kernelsu.ko"
echo "Built ${output_dir}/kernelsu.ko for ${kernel_release}"
