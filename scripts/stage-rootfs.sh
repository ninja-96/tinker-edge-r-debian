#!/usr/bin/env bash
# Stage the complete root filesystem tree into <dir>: Debian tarball, our kernel
# and modules, device tree variants, overlay, NPU host files.
# Shared by build-image.sh, build-image-mainline-uboot.sh and qemu-smoke.sh.
#   stage-rootfs.sh <dir>
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
W=${1:?destination dir}
O=out
KVER=${LINUX_VERSION}${KERNEL_LOCALVERSION}
NPU_SRC=downloads/RK3399Pro_npu

rm -rf "$W"; mkdir -p "$W"
tar -xpf $O/rootfs.tar -C "$W" --numeric-owner

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

# overlay (services, helper scripts, udev rules) and unit enablement
cp -a overlay/. "$W/"
mkdir -p "$W/etc/systemd/system/multi-user.target.wants"
for u in npu-firmware npu-transfer-proxy tinker-expand-rootfs ssh-hostkeys; do
    ln -sf "/etc/systemd/system/$u.service" "$W/etc/systemd/system/multi-user.target.wants/$u.service"
done

# NPU host side: open Apache-2.0 set from airockchip/RK3399Pro_npu ...
if [ -d "$NPU_SRC/drivers" ]; then
    install -D -m 0755 $NPU_SRC/drivers/npu_transfer_proxy/linux-aarch64/npu_transfer_proxy "$W/usr/bin/npu_transfer_proxy"
    mkdir -p "$W/usr/share/npu_fw" "$W/usr/share/npu_fw_pcie"
    cp $NPU_SRC/drivers/npu_firmware/npu_fw/*      "$W/usr/share/npu_fw/"
    cp $NPU_SRC/drivers/npu_firmware/npu_pcie_fw/* "$W/usr/share/npu_fw_pcie/"
    install -D -m 0644 $NPU_SRC/rknn-api/librknn_api/Linux/lib64/librknn_api.so "$W/usr/lib/aarch64-linux-gnu/librknn_api.so"
    install -D -m 0644 $NPU_SRC/rknn-api/librknn_api/include/rknn_api.h "$W/usr/include/rknn_api.h"
fi
# ... overridden by the user's own ASUS copies (matched proxy+firmware pair and
# upgrade_tool, which is not redistributable) when extract-stock-boot.sh ran
if [ -d $O/stock-npu ]; then
    [ -f $O/stock-npu/upgrade_tool ] && install -D -m 0755 $O/stock-npu/upgrade_tool "$W/usr/bin/upgrade_tool"
    [ -f $O/stock-npu/npu_transfer_proxy ] && install -D -m 0755 $O/stock-npu/npu_transfer_proxy "$W/usr/bin/npu_transfer_proxy"
    [ -d $O/stock-npu/npu_fw ] && cp $O/stock-npu/npu_fw/* "$W/usr/share/npu_fw/"
    [ -d $O/stock-npu/npu_fw_pcie ] && cp $O/stock-npu/npu_fw_pcie/* "$W/usr/share/npu_fw_pcie/"
fi
exit 0
