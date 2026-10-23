#!/usr/bin/env bash
# Stage the complete root filesystem tree into <dir>: Debian tarball, our kernel
# and modules, device tree variants, overlay, NPU host files.
# FREE=1: use out/rootfs-free.tar and add no closed binaries (no NPU userspace).
# Shared by build-image.sh, build-image-mainline-uboot.sh and qemu-smoke.sh.
#   stage-rootfs.sh <dir>
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
W=${1:?destination dir}
O=out
KVER=${LINUX_VERSION}${KERNEL_LOCALVERSION}
FREE=${FREE:-0}
TAR=$O/rootfs.tar
[ "$FREE" = 1 ] && TAR=$O/rootfs-free.tar

rm -rf "$W"; mkdir -p "$W"
tar -xpf "$TAR" -C "$W" --numeric-owner

# no SSH host keys in the image: ssh-hostkeys.service creates them on first boot
rm -f "$W"/etc/ssh/ssh_host_*

# kernel modules
mkdir -p "$W/lib/modules"
cp -a "$O/modules/lib/modules/$KVER" "$W/lib/modules/"
rm -f "$W/lib/modules/$KVER/build" "$W/lib/modules/$KVER/source"

# kernel + all device tree variants (tinker-boot-select repacks the boot partition)
mkdir -p "$W/boot/dtbs"
cp $O/Image "$W/boot/Image"
cp $O/rk3399pro-tinker-edge-r*.dtb "$W/boot/dtbs/"
install -D -m 0755 scripts/mkbootimg.py "$W/usr/local/sbin/tinker-mkbootimg"

# Rockchip mpp library and test tools (built by build-mpp.sh; used with the -mpp device tree)
if [ -d "$O/mpp/usr" ]; then
    mkdir -p "$W/usr"
    cp -a "$O/mpp/usr/." "$W/usr/"
fi

# Rockchip GStreamer plugins (built by build-gst-rockchip.sh)
if [ -d "$O/gst-rockchip/usr" ]; then
    cp -a "$O/gst-rockchip/usr/." "$W/usr/"
fi

# overlay (services, helper scripts, udev rules) and unit enablement
cp -a overlay/. "$W/"
mkdir -p "$W/etc/systemd/system/multi-user.target.wants"
for u in npu-firmware npu-transfer-proxy tinker-expand-rootfs ssh-hostkeys; do
    ln -sf "/etc/systemd/system/$u.service" "$W/etc/systemd/system/multi-user.target.wants/$u.service"
done

# NPU host side (experimental, closed binaries, NOT shipped or downloaded): if you put
# the files into third_party/npu/ they are installed. Expected layout, see docs/NPU.md:
#   npu_transfer_proxy  upgrade_tool  librknn_api.so  rknn_api.h  npu_fw/  npu_fw_pcie/
N=third_party/npu
if [ "$FREE" != 1 ] && [ -d "$N" ]; then
    for f in npu_transfer_proxy upgrade_tool; do
        [ -f "$N/$f" ] && install -D -m 0755 "$N/$f" "$W/usr/bin/$f"
    done
    [ -f "$N/librknn_api.so" ] && install -D -m 0644 "$N/librknn_api.so" "$W/usr/lib/aarch64-linux-gnu/librknn_api.so"
    [ -f "$N/rknn_api.h" ]     && install -D -m 0644 "$N/rknn_api.h" "$W/usr/include/rknn_api.h"
    for d in npu_fw npu_fw_pcie; do
        [ -d "$N/$d" ] && { mkdir -p "$W/usr/share/$d"; cp "$N/$d"/* "$W/usr/share/$d/"; }
    done
fi
exit 0
