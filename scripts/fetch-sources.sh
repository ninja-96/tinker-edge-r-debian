#!/usr/bin/env bash
# Download pinned upstream sources into ./downloads (ignored by git).
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
D=downloads
mkdir -p "$D"

clone_at() { # name repo rev [sparse paths...]
    local name=$1 repo=$2 rev=$3; shift 3
    if [ ! -d "$D/$name/.git" ]; then
        git clone --filter=blob:none --no-checkout "$repo" "$D/$name"
    fi
    if [ $# -gt 0 ]; then
        git -C "$D/$name" sparse-checkout set "$@"
    fi
    git -C "$D/$name" fetch --quiet origin "$rev" 2>/dev/null || git -C "$D/$name" fetch --quiet origin
    git -C "$D/$name" checkout --quiet "$rev"
    echo "$name @ $(git -C "$D/$name" rev-parse --short HEAD)"
}

# Linux
tarball="linux-${LINUX_VERSION}.tar.xz"
if [ ! -f "$D/$tarball" ]; then
    curl -fL -o "$D/$tarball" "https://cdn.kernel.org/pub/linux/kernel/v${LINUX_VERSION%%.*}.x/$tarball"
fi
echo "$LINUX_SHA256  $D/$tarball" | sha256sum -c -

# U-Boot (tag)
if [ ! -d "$D/u-boot/.git" ]; then
    git clone --depth 1 --branch "$UBOOT_TAG" "$UBOOT_REPO" "$D/u-boot"
fi
echo "u-boot @ $(git -C "$D/u-boot" describe --tags)"

clone_at rkbin           "$RKBIN_REPO"           "$RKBIN_REV"
clone_at mpp             "$MPP_REPO"             "$MPP_REV"
clone_at RK3399Pro_npu   "$RK3399PRO_NPU_REPO"   "$RK3399PRO_NPU_REV"
clone_at rkdeveloptool   "$RKDEVELOPTOOL_REPO"   "$RKDEVELOPTOOL_REV"
clone_at rk-kernel       "$RK_KERNEL_REPO"       "$RK_KERNEL_REV" \
    drivers/video/rockchip/mpp drivers/pci/controller drivers/soc/rockchip \
    arch/arm64/boot/dts/rockchip include/soc/rockchip
