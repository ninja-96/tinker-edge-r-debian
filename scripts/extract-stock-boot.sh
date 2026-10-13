#!/usr/bin/env bash
# Extract the bootloader pieces from YOUR copy of the ASUS TinkerOS image.
# They are ASUS/Rockchip binaries and are not redistributed in this repo.
#   scripts/extract-stock-boot.sh Tinker_Edge_R-Debian-Buster-v2.0.5-20220217.zip
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${1:?usage: $0 <ASUS image .zip or .img>}
OUT=out/stock-boot
mkdir -p "$OUT" downloads

IMG=$SRC
if [[ $SRC == *.zip ]]; then
    IMG=downloads/$(unzip -Z1 "$SRC" | grep '\.img$' | head -1)
    [ -f "$IMG" ] || unzip -o -q -d downloads "$SRC"
fi

# Rockchip layout: idbloader @64, uboot @16384, trust @24576 (sectors, 512 B)
dd if="$IMG" of="$OUT/idbloader.bin" bs=512 skip=64    count=16320 status=none
dd if="$IMG" of="$OUT/uboot.img"     bs=512 skip=16384 count=8192  status=none
dd if="$IMG" of="$OUT/trust.img"     bs=512 skip=24576 count=8192  status=none
ls -l "$OUT"

# NPU host tools and the ASUS-matched firmware (optional; needs debugfs + sgdisk)
if command -v debugfs >/dev/null && command -v sgdisk >/dev/null; then
    start=$(sgdisk -i 8 "$IMG" | awk '/First sector/ {print $3}')
    R="$IMG?offset=$(( start * 512 ))"
    N=out/stock-npu
    rm -rf "$N"; mkdir -p "$N"
    for f in upgrade_tool npu_transfer_proxy; do
        debugfs -R "dump /usr/bin/$f $N/$f" "$R" 2>/dev/null
    done
    for d in npu_fw npu_fw_pcie; do
        debugfs -R "rdump /usr/share/$d $N" "$R" 2>/dev/null
    done
    ls -l "$N"
fi
