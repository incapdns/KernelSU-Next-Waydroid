#!/usr/bin/env bash
set -euo pipefail

kernel_release=${KERNEL_RELEASE:-$(uname -r)}
kernel_build=${KERNEL_BUILD:-/usr/lib/modules/${kernel_release}/build}
repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
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
    CONFIG_KSU_NON_ANDROID=y \
    CONFIG_KSU_SELINUX=n \
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
