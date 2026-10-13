#!/usr/bin/env bash
# Assemble the SD card image out/tinker-edge-r-debian13.img
# Needs: out/Image, out/rk3399pro-tinker-edge-r.dtb, out/modules, out/rootfs.tar,
#        out/stock-boot/{idbloader.bin,uboot.img,trust.img}
# Run inside the builder container:  ./run.sh scripts/build-image.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
O=out
ROOTFS_MIB=${ROOTFS_MIB:-1536}
KVER=${LINUX_VERSION}-tinker

DTB_VARIANT=${DTB_VARIANT:-base}   # base | mpp | npu | full
case "$DTB_VARIANT" in
    base) BOOT_DTB=$O/rk3399pro-tinker-edge-r.dtb ;;
    *)    BOOT_DTB=$O/rk3399pro-tinker-edge-r-$DTB_VARIANT.dtb ;;
esac
IMG=$O/tinker-edge-r-debian13${DTB_VARIANT:+-$DTB_VARIANT}.img
[ "$DTB_VARIANT" = base ] && IMG=$O/tinker-edge-r-debian13.img

for f in $O/Image $BOOT_DTB $O/rootfs.tar \
         $O/stock-boot/idbloader.bin $O/stock-boot/uboot.img $O/stock-boot/trust.img; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
[ -d "$O/modules/lib/modules/$KVER" ] || { echo "missing modules for $KVER" >&2; exit 1; }

# --- boot partition payload: Android boot.img with Image + resource(dtb)
python3 scripts/mkbootimg.py --kernel $O/Image --dtb $BOOT_DTB -o $O/boot.img

# --- rootfs: staged tree packed as ext4 (no loop mount needed)
W=build/rootfs
scripts/stage-rootfs.sh "$W"
mke2fs -q -t ext4 -L rootfs -d "$W" -E root_owner=0:0 $O/rootfs.ext4 ${ROOTFS_MIB}M
e2fsck -fn $O/rootfs.ext4 >/dev/null

# --- disk: Rockchip GPT layout like the stock image, with a bigger boot
#     partition (rootfs must stay p8: the ASUS U-Boot hardcodes
#     root=/dev/mmcblk0p8). U-Boot finds partitions by name.
S=512
ROOT_START=221184
ROOT_SECT=$(( ROOTFS_MIB * 1024 * 1024 / S ))
TOTAL=$(( ROOT_START + ROOT_SECT + 33 ))
rm -f "$IMG"; truncate -s $(( TOTAL * S )) "$IMG"
L=0FC63DAF-8483-4772-8E79-3D69D8477DE4
sfdisk -q "$IMG" <<EOT
label: gpt
first-lba: 34
unit: sectors

start=16384,  size=8192,   type=$L, name="uboot"
start=24576,  size=8192,   type=$L, name="trust"
start=32768,  size=8192,   type=$L, name="misc"
start=40960,  size=131072, type=$L, name="boot"
start=172032, size=16384,  type=$L, name="recovery"
start=188416, size=16384,  type=$L, name="backup"
start=204800, size=16384,  type=$L, name="userdata"
start=$ROOT_START, size=$ROOT_SECT, type=$L, name="rootfs"
EOT

w() { dd if="$1" of="$IMG" bs=512 seek="$2" conv=notrunc status=none; }
w $O/stock-boot/idbloader.bin 64
w $O/stock-boot/uboot.img     16384
w $O/stock-boot/trust.img     24576
w $O/boot.img                 40960
w $O/rootfs.ext4              $ROOT_START

[ "$(stat -c %s $O/boot.img)" -le $(( 131072 * S )) ] || { echo "boot.img too large for boot partition" >&2; exit 1; }
chown "${HOST_UID:-0}:${HOST_GID:-0}" $O/*.img $O/rootfs.ext4 2>/dev/null || true
sfdisk -l "$IMG" | tail -12
ls -lh "$IMG"
