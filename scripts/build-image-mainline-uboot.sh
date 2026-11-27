#!/usr/bin/env bash
# THE image for the Tinker Edge R: mainline U-Boot + extlinux (this is the one that
# boots on the board). Output: out/tinker-edge-r-debian13.img
# FREE=1 builds the fully open variant (no closed binaries, Debian "main" only):
#   needs out/rootfs-free.tar and out/u-boot-rockchip.bin built with the open TF-A.
# Also the fallback if the stock bootloader refuses the mainline kernel. Untested.
# Needs out/u-boot-rockchip.bin (scripts/build-uboot.sh), out/Image, DTBs,
# out/modules, out/rootfs.tar. Output: out/tinker-edge-r-debian13-mainline-uboot.img
# Run inside the builder container:  ./run.sh scripts/build-image-mainline-uboot.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
O=out
KVER=${LINUX_VERSION}${KERNEL_LOCALVERSION}
# browser (default, release 0.6): hybrid codecs — the mainline rkvdec (V4L2 stateless)
# decodes for the browser through the v4l2_request VA-API driver, the mpp vepu/vdpu keep
# GStreamer's H.264/VP8/JPEG encoding and H.264 decode; full/mpp: every codec through
# mpp_service (no browser hardware decode); base: mainline V4L2 only, no encoder.
DTB_VARIANT=${DTB_VARIANT:-browser}
case "$DTB_VARIANT" in
    base) DTB=rk3399pro-tinker-edge-r.dtb ;;
    *)    DTB=rk3399pro-tinker-edge-r-$DTB_VARIANT.dtb ;;
esac
FREE=${FREE:-0}
IMG=$O/tinker-edge-r-debian13.img
[ "$FREE" = 1 ] && IMG=$O/tinker-edge-r-debian13-free.img
ROOTFS_MIB=${ROOTFS_MIB:-2560}
BOOT_MIB=64
PARTUUID_ROOT=6c1f7b3e-0b6c-4a7e-9d5c-2b1c3f4a5e02
ROOT_LABEL=rootfs
ROOTFS_IMG=$O/rootfs.ext4
MKFS_ARGS=()
if [ "${SD:-0}" = 1 ]; then
    IMG=${IMG%.img}-sd.img
    PARTUUID_ROOT=be89af21-b965-42a8-8b50-71a91f85dcb4
    ROOT_LABEL=rootfs-sd
    ROOTFS_IMG=$O/rootfs-sd.ext4
    MKFS_ARGS=(-m 0)
fi

TARFILE=$O/rootfs.tar; [ "$FREE" = 1 ] && TARFILE=$O/rootfs-free.tar
# Default loader: the variant with the Rockchip DDR blob (U-Boot's open LPDDR4 TPL
# hangs on this board), else the all-open one.
UBOOT_BIN=${UBOOT_BIN:-$O/u-boot-rockchip-ddrblob.bin}
[ -f "$UBOOT_BIN" ] || UBOOT_BIN=$O/u-boot-rockchip.bin
[ "$FREE" = 1 ] && [[ "$UBOOT_BIN" == *ddrblob* ]] && { echo "the free image must use the open U-Boot: set UBOOT_BIN=$O/u-boot-rockchip.bin" >&2; exit 1; }
for f in "$UBOOT_BIN" $O/Image $O/$DTB "$TARFILE"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

W=build/rootfs-mu
scripts/stage-rootfs.sh "$W"
mkdir -p "$W/boot/extlinux"
sed -i "s/^LABEL=rootfs /LABEL=$ROOT_LABEL /" "$W/etc/fstab"

# extlinux on the rootfs partition itself (U-Boot reads /boot/extlinux/extlinux.conf)
cat > "$W/boot/extlinux/extlinux.conf" <<EOT
default tinker
timeout 20

label tinker
    kernel /boot/Image
    fdt /boot/dtbs/$DTB
    append earlycon=uart8250,mmio32,0xff180000 console=tty1 console=ttyS0,115200n8 root=PARTUUID=$PARTUUID_ROOT rw rootfstype=ext4 rootwait coherent_pool=1m fbcon=logo-pos:center fbcon=logo-count:1 quiet splash
EOT

mke2fs -q -t ext4 "${MKFS_ARGS[@]}" -L "$ROOT_LABEL" -d "$W" -E root_owner=0:0 "$ROOTFS_IMG" ${ROOTFS_MIB}M
e2fsck -fn "$ROOTFS_IMG" >/dev/null

S=512
ROOT_START=32768                       # 16 MiB: after the 8 MiB U-Boot area
[ "$(stat -c %s "$UBOOT_BIN")" -le $(((ROOT_START-64)*S)) ] || { echo "loader exceeds the reserved boot area" >&2; exit 1; }
ROOT_SECT=$(( ROOTFS_MIB * 1024 * 1024 / S ))
TOTAL=$(( ROOT_START + ROOT_SECT + 33 ))
rm -f "$IMG"; truncate -s $(( TOTAL * S )) "$IMG"
sfdisk -q "$IMG" <<EOT
label: gpt
first-lba: 34
unit: sectors

start=$ROOT_START, size=$ROOT_SECT, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=$PARTUUID_ROOT, name="rootfs", attrs="LegacyBIOSBootable"
EOT
# Both images contain U-Boot so SD can also boot when eMMC has no loader.
dd if="$UBOOT_BIN" of="$IMG" bs=512 seek=64 conv=notrunc status=none
dd if="$ROOTFS_IMG" of="$IMG" bs=512 seek=$ROOT_START conv=notrunc status=none
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$IMG" "$ROOTFS_IMG" 2>/dev/null || true
sfdisk -l "$IMG" | tail -4
ls -lh "$IMG"

if [ "${SD:-0}" = 1 ]; then
    LOADER_IMG=$O/tinker-edge-r-uboot-emmc.img
    [ "$FREE" = 1 ] && LOADER_IMG=$O/tinker-edge-r-uboot-emmc-free.img
    # No partition table or OS: the loader is at its normal absolute eMMC LBAs.
    truncate -s 0 "$LOADER_IMG"
    truncate -s $((ROOT_START*S)) "$LOADER_IMG"
    dd if="$UBOOT_BIN" of="$LOADER_IMG" bs=512 seek=64 conv=notrunc status=none
    chown "${HOST_UID:-0}:${HOST_GID:-0}" "$LOADER_IMG" 2>/dev/null || true
    ls -lh "$LOADER_IMG"
fi
