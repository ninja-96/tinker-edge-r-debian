#!/usr/bin/env bash
# FALLBACK image: mainline U-Boot + extlinux instead of the stock ASUS U-Boot.
# Use it if the stock bootloader refuses the mainline kernel. Untested.
# Needs out/u-boot-rockchip.bin (scripts/build-uboot.sh), out/Image, DTBs,
# out/modules, out/rootfs.tar. Output: out/tinker-edge-r-debian13-mainline-uboot.img
# Run inside the builder container:  ./run.sh scripts/build-image-mainline-uboot.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
O=out
KVER=${LINUX_VERSION}-tinker
DTB_VARIANT=${DTB_VARIANT:-base}
case "$DTB_VARIANT" in
    base) DTB=rk3399pro-tinker-edge-r.dtb ;;
    *)    DTB=rk3399pro-tinker-edge-r-$DTB_VARIANT.dtb ;;
esac
IMG=$O/tinker-edge-r-debian13-mainline-uboot.img
ROOTFS_MIB=${ROOTFS_MIB:-1536}
BOOT_MIB=64
PARTUUID_ROOT=6c1f7b3e-0b6c-4a7e-9d5c-2b1c3f4a5e02

for f in $O/u-boot-rockchip.bin $O/Image $O/$DTB $O/rootfs.tar; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

W=build/rootfs-mu
scripts/stage-rootfs.sh "$W"
mkdir -p "$W/boot/extlinux"

# extlinux on the rootfs partition itself (U-Boot reads /boot/extlinux/extlinux.conf)
cat > "$W/boot/extlinux/extlinux.conf" <<EOT
default tinker
timeout 20

label tinker
    kernel /boot/Image
    fdt /boot/dtbs/$DTB
    append earlycon=uart8250,mmio32,0xff1a0000 console=tty1 console=ttyS2,115200n8 root=PARTUUID=$PARTUUID_ROOT rw rootfstype=ext4 rootwait coherent_pool=1m
EOT

mke2fs -q -t ext4 -L rootfs -d "$W" -E root_owner=0:0 $O/rootfs.ext4 ${ROOTFS_MIB}M
e2fsck -fn $O/rootfs.ext4 >/dev/null

S=512
ROOT_START=32768                       # 16 MiB: after the 8 MiB U-Boot area
ROOT_SECT=$(( ROOTFS_MIB * 1024 * 1024 / S ))
TOTAL=$(( ROOT_START + ROOT_SECT + 33 ))
rm -f "$IMG"; truncate -s $(( TOTAL * S )) "$IMG"
sfdisk -q "$IMG" <<EOT
label: gpt
first-lba: 34
unit: sectors

start=$ROOT_START, size=$ROOT_SECT, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=$PARTUUID_ROOT, name="rootfs", attrs="LegacyBIOSBootable"
EOT
dd if=$O/u-boot-rockchip.bin of="$IMG" bs=512 seek=64 conv=notrunc status=none
dd if=$O/rootfs.ext4 of="$IMG" bs=512 seek=$ROOT_START conv=notrunc status=none
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$IMG" $O/rootfs.ext4 2>/dev/null || true
sfdisk -l "$IMG" | tail -4
ls -lh "$IMG"
