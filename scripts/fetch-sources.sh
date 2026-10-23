#!/usr/bin/env bash
# Download the pinned open-source upstream sources into ./downloads (git-ignored):
# Linux (kernel.org tarball, sha256 checked) and U-Boot. Nothing from ASUS or Rockchip.
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
D=downloads
mkdir -p "$D"

# Linux
tarball="linux-${LINUX_VERSION}.tar.xz"
if [ ! -f "$D/$tarball" ]; then
    curl -fL -o "$D/$tarball" "https://cdn.kernel.org/pub/linux/kernel/v${LINUX_VERSION%%.*}.x/$tarball"
fi
echo "${LINUX_SHA256}  $D/$tarball" | sha256sum -c -

# U-Boot (tag)
if [ ! -d "$D/u-boot/.git" ]; then
    git clone --depth 1 --branch "$UBOOT_TAG" "$UBOOT_REPO" "$D/u-boot"
fi
echo "u-boot @ $(git -C "$D/u-boot" describe --tags)"
