#!/usr/bin/env bash
# Download the Rockchip NPU files (Apache-2.0 repository airockchip/RK3399Pro_npu, binaries as they
# are) at the commit pinned in sources.lock and check their sha256. Result in downloads/npu/:
#   npu_fw/{MiniLoaderAll.bin,uboot.img,trust.img,boot.img}   firmware of the NPU die
#   npu_transfer_proxy  librknn_api.so  rknn_api.h  LICENSE
# stage-rootfs.sh installs them into the image (skipped with FREE=1).
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
    g sparse-checkout set --no-cone /LICENSE /drivers/npu_firmware/npu_fw /drivers/npu_transfer_proxy/linux-aarch64 \
        /rknn-api/librknn_api/Linux/lib64 /rknn-api/librknn_api/include
    g checkout -q --force FETCH_HEAD
fi
echo "RK3399Pro_npu @ $(g rev-parse --short HEAD)"

SUMS=$(cat <<'S'
e554a1510ec462a31242c99b87b5ad83fbccdb570f33db8305cb3d9ff3fa6332  drivers/npu_firmware/npu_fw/MiniLoaderAll.bin
e3246735294a5cae0f0cb3a9d9ad5b16a0663815e202e366220d9cd6c7c3e3ca  drivers/npu_firmware/npu_fw/uboot.img
92d650fe50a9a53e638ebd80d25d793fc298201675ab8cd1370d7593c59a5be2  drivers/npu_firmware/npu_fw/trust.img
7cef42d2c487daccd38b9b8b67f6eb3ee40b8d853bf7185a1b30c673a089df29  drivers/npu_firmware/npu_fw/boot.img
c6961f392030a71272e53263246f688ad74ea7410d302c9ba9988cd5e5d30b68  drivers/npu_transfer_proxy/linux-aarch64/npu_transfer_proxy
63957db5cbc82d29b7969d44f17d8da22beb2cab39f011c4864235410b8dfae8  rknn-api/librknn_api/Linux/lib64/librknn_api.so
c787d48c9e6f1fe0f23af9319b9ac8cea6413259761c7f4c32348ff493fffa72  rknn-api/librknn_api/include/rknn_api.h
c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4  LICENSE
S
)
( cd "$R" && echo "$SUMS" | sha256sum -c - >/dev/null ) || { echo "NPU files: checksum mismatch" >&2; exit 1; }

rm -rf "$D/npu_fw"; mkdir -p "$D/npu_fw"
cp "$R"/drivers/npu_firmware/npu_fw/{MiniLoaderAll.bin,uboot.img,trust.img,boot.img} "$D/npu_fw/"
cp "$R/drivers/npu_transfer_proxy/linux-aarch64/npu_transfer_proxy" "$D/"
cp "$R/rknn-api/librknn_api/Linux/lib64/librknn_api.so" "$R/rknn-api/librknn_api/include/rknn_api.h" "$R/LICENSE" "$D/"
chmod 0755 "$D/npu_transfer_proxy"
echo "NPU files verified and placed in $D"
