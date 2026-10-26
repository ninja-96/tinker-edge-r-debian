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

# Comfortable Tab completion in the shell: bash-completion (installed in the rootfs) covers
# commands such as apt, systemctl, ssh, git; here the readline switches that Debian ships
# commented out in /etc/inputrc are turned on: show all matches at once on the first Tab,
# ignore case, and search the history by prefix with PageUp / PageDown.
if [ -f "$W/etc/inputrc" ]; then
    sed -i -E \
        -e 's/^# *set (completion-ignore-case) On/set \1 On/' \
        -e 's/^# *set (show-all-if-ambiguous) On/set \1 On/' \
        -e 's/^# *("\\e\[5~": history-search-backward)/\1/' \
        -e 's/^# *("\\e\[6~": history-search-forward)/\1/' \
        "$W/etc/inputrc"
    printf '%s\n' 'set colored-stats On' 'set mark-symlinked-directories On' >> "$W/etc/inputrc"
fi

# overlay (services, helper scripts, udev rules) and unit enablement
cp -a --no-preserve=ownership overlay/. "$W/"

# Licences, the notice and the package manifest travel with the image (see NOTICE.md, section 4)
DOC="$W/usr/share/doc/tinker-edge-r"
mkdir -p "$DOC/licenses"
cp NOTICE.md "$DOC/NOTICE.md"
cp LICENSES/*.txt "$DOC/licenses/"
cp blobs/rockchip/LICENSE "$DOC/licenses/Rockchip-rkbin-LICENSE"
cp blobs/README.md "$DOC/blobs-README.md"
[ -d downloads/mpp/LICENSES ] && cp -a --no-preserve=ownership downloads/mpp/LICENSES "$DOC/licenses/mpp"
[ -f downloads/gst-rockchip/COPYING ] && cp downloads/gst-rockchip/COPYING "$DOC/licenses/gst-rockchip-COPYING"
dpkg-query --admindir="$W/var/lib/dpkg" -W -f='${Package}\t${Version}\t${Architecture}\t${Source}\n' | sort > "$DOC/packages.txt"
cp "$DOC/packages.txt" "$O/packages.txt"
mkdir -p "$W/etc/systemd/system/multi-user.target.wants"
for u in npu-firmware npu-transfer-proxy tinker-expand-rootfs ssh-hostkeys tinker-power; do
    ln -sf "/etc/systemd/system/$u.service" "$W/etc/systemd/system/multi-user.target.wants/$u.service"
done

# NPU host side. Open parts: rkdeveloptool with the 'rs' patch (loads the NPU firmware), the
# Python module rk3399pro_npu. Closed Rockchip files (Apache-2.0 repository airockchip/RK3399Pro_npu, as they
# are; scripts/fetch-npu.sh downloads and verifies them): firmware of the NPU die, npu_transfer_proxy,
# librknn_api.so. Not installed with FREE=1. A third_party/npu/ directory (git-ignored, same layout)
# overrides downloads/npu/.
[ -x "$O/rkdeveloptool-arm64/usr/bin/rkdeveloptool" ] && install -D -m 0755 "$O/rkdeveloptool-arm64/usr/bin/rkdeveloptool" "$W/usr/bin/rkdeveloptool"
if [ -d python/rk3399pro_npu ]; then
    mkdir -p "$W/usr/lib/python3/dist-packages"
    rm -rf "$W/usr/lib/python3/dist-packages/rk3399pro_npu"
    cp -a --no-preserve=ownership python/rk3399pro_npu "$W/usr/lib/python3/dist-packages/"
    find "$W/usr/lib/python3/dist-packages/rk3399pro_npu" -name __pycache__ -prune -exec rm -rf {} +
fi
N=downloads/npu; [ -d third_party/npu ] && N=third_party/npu
if [ "${FREE:-0}" != 1 ] && [ -d "$N" ]; then
    [ -f "$N/npu_transfer_proxy" ] && install -D -m 0755 "$N/npu_transfer_proxy" "$W/usr/bin/npu_transfer_proxy"
    [ -f "$N/librknn_api.so" ] && install -D -m 0644 "$N/librknn_api.so" "$W/usr/lib/aarch64-linux-gnu/librknn_api.so"
    [ -f "$N/rknn_api.h" ]     && install -D -m 0644 "$N/rknn_api.h" "$W/usr/include/rknn_api.h"
    [ -f "$N/LICENSE" ]        && install -D -m 0644 "$N/LICENSE" "$DOC/licenses/RK3399Pro_npu-LICENSE"
    if [ -d "$N/npu_fw" ]; then
        mkdir -p "$W/usr/share/npu_fw"; cp "$N/npu_fw"/* "$W/usr/share/npu_fw/"
        # Rockchip's image expects the regulators of their EVB; only its device tree is adapted to this board
        # (the ASUS image from third_party/npu/ does not need it and is left as it is)
        python3 scripts/npu-fw-tinker.py "$N/npu_fw/boot.img" "$W/usr/share/npu_fw/boot.img" \
            || cp "$N/npu_fw/boot.img" "$W/usr/share/npu_fw/boot.img"
    fi
fi
exit 0
