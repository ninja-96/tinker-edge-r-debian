#!/usr/bin/env bash
# Download the firmware of the NPU die (Apache-2.0 repository airockchip/RK3399Pro_npu, binaries as they
# are) at the commit pinned in sources.lock and check their sha256. Result in downloads/npu/:
#   npu_fw/{MiniLoaderAll.bin,uboot.img,trust.img,boot.img}   firmware of the NPU die
#   LICENSE
# scripts/build-npu-boot.sh puts our server into its boot.img; stage-rootfs.sh installs the result into the image (skipped with FREE=1).
# Release 0.4 no longer uses Rockchip's npu_transfer_proxy, librknn_api or rknn_api.h.
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
D=downloads/npu
R=$D/src
g() { git -c safe.directory="*" -C "$R" "$@"; }

if [ ! -d "$R/.git" ]; then
    mkdir -p "$R"
    git init -q "$R"
    g remote add origin "$RK3399PRO_NPU_REPO"
fi
if [ "$(g rev-parse HEAD 2>/dev/null || true)" != "$RK3399PRO_NPU_REV" ]; then
    g fetch -q --depth 1 --filter=blob:none origin "$RK3399PRO_NPU_REV"
    g sparse-checkout set --no-cone /LICENSE /drivers/npu_firmware/npu_fw
    g checkout -q --force FETCH_HEAD
fi
echo "RK3399Pro_npu @ $(g rev-parse --short HEAD)"

SUMS=$(cat <<'S'
e554a1510ec462a31242c99b87b5ad83fbccdb570f33db8305cb3d9ff3fa6332  drivers/npu_firmware/npu_fw/MiniLoaderAll.bin
e3246735294a5cae0f0cb3a9d9ad5b16a0663815e202e366220d9cd6c7c3e3ca  drivers/npu_firmware/npu_fw/uboot.img
92d650fe50a9a53e638ebd80d25d793fc298201675ab8cd1370d7593c59a5be2  drivers/npu_firmware/npu_fw/trust.img
7cef42d2c487daccd38b9b8b67f6eb3ee40b8d853bf7185a1b30c673a089df29  drivers/npu_firmware/npu_fw/boot.img
c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4  LICENSE
S
)
( cd "$R" && echo "$SUMS" | sha256sum -c - >/dev/null ) || { echo "NPU files: checksum mismatch" >&2; exit 1; }

rm -rf "$D/npu_fw"; mkdir -p "$D/npu_fw"
cp "$R"/drivers/npu_firmware/npu_fw/{MiniLoaderAll.bin,uboot.img,trust.img,boot.img} "$D/npu_fw/"
cp "$R/LICENSE" "$D/"
rm -f "$D/npu_transfer_proxy" "$D/librknn_api.so" "$D/rknn_api.h"   # left by an older version of this script
echo "NPU files verified and placed in $D"
