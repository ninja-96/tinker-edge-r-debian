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
cp -a --no-preserve=ownership "$O/modules/lib/modules/$KVER" "$W/lib/modules/"
rm -f "$W/lib/modules/$KVER/build" "$W/lib/modules/$KVER/source"

# kernel + all device tree variants (tinker-boot-select repacks the boot partition)
mkdir -p "$W/boot/dtbs"
cp $O/Image "$W/boot/Image"
cp $O/rk3399pro-tinker-edge-r*.dtb "$W/boot/dtbs/"
install -D -m 0755 scripts/mkbootimg.py "$W/usr/local/sbin/tinker-mkbootimg"

# Rockchip mpp library and test tools (built by build-mpp.sh; used with the -mpp device tree)
if [ -d "$O/mpp/usr" ]; then
    mkdir -p "$W/usr"
    cp -a --no-preserve=ownership "$O/mpp/usr/." "$W/usr/"
fi

# Rockchip GStreamer plugins (built by build-gst-rockchip.sh)
if [ -d "$O/gst-rockchip/usr" ]; then
    cp -a --no-preserve=ownership "$O/gst-rockchip/usr/." "$W/usr/"
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
for u in npu-firmware npu-daemon tinker-expand-rootfs ssh-hostkeys tinker-power; do
    ln -sf "/etc/systemd/system/$u.service" "$W/etc/systemd/system/multi-user.target.wants/$u.service"
done

# NPU host side. Open parts: rkdeveloptool with the 'rs' patch (loads the firmware into the NPU die), the C++ library librk3399pro_npu with its
# Python module rk3399pro_npu (TensorFlow Lite models over our own USB channel), the daemon npu-daemon that shares the NPU between programs, npu-info,
# npu-models (scripts/build-npu-host.sh). The firmware image of the NPU die (out/npu_fw/, scripts/build-npu-boot.sh) is Rockchip's
# (closed, Apache-2.0 repository airockchip/RK3399Pro_npu, as it is except for the device tree) with our open server added: TIM-VX, the TFLite VX
# delegate and npu_server (scripts/build-npu-open.sh). Not installed with FREE=1.
[ -x "$O/rkdeveloptool-arm64/usr/bin/rkdeveloptool" ] && install -D -m 0755 "$O/rkdeveloptool-arm64/usr/bin/rkdeveloptool" "$W/usr/bin/rkdeveloptool"
if [ -d python/rk3399pro_npu ]; then
    mkdir -p "$W/usr/lib/python3/dist-packages"
    rm -rf "$W/usr/lib/python3/dist-packages/rk3399pro_npu"
    cp -a --no-preserve=ownership python/rk3399pro_npu "$W/usr/lib/python3/dist-packages/"
    find "$W/usr/lib/python3/dist-packages/rk3399pro_npu" -name __pycache__ -prune -exec rm -rf {} +
fi
H=$O/npu-host
if [ -f "$H/npu-daemon" ]; then
    install -D -m 0755 "$H/npu-daemon" "$W/usr/sbin/npu-daemon"
    install -D -m 0644 "$H"/_core.cpython-*-aarch64-linux-gnu.so -t "$W/usr/lib/python3/dist-packages/rk3399pro_npu/"
    install -D -m 0644 "$H/librk3399pro_npu.so.0" "$W/usr/lib/aarch64-linux-gnu/librk3399pro_npu.so.0"
    ln -sf librk3399pro_npu.so.0 "$W/usr/lib/aarch64-linux-gnu/librk3399pro_npu.so"
    install -D -m 0644 "$H/rk3399pro_npu.hpp" "$W/usr/include/rk3399pro_npu.hpp"
    [ -d "$H/licenses" ] && install -D -m 0644 "$H"/licenses/* -t "$DOC/licenses/"
else
    echo "WARNING: out/npu-host missing (./run.sh scripts/build-npu-host.sh): the image gets no NPU library and no npu-daemon" >&2
fi
cp docs/NPU.md npu-open/OPERATORS.md "$DOC/"
mkdir -p "$W/usr/share/tinker-edge-r/npu-selftest"
cp tests/npu/net_int8.tflite tests/npu/net_uint8.tflite tests/npu/expected.npz "$W/usr/share/tinker-edge-r/npu-selftest/"   # for /usr/local/bin/npu-selftest
if [ "${FREE:-0}" != 1 ]; then
    if [ -f "$O/npu_fw/boot.img" ]; then
        mkdir -p "$W/usr/share/npu_fw"; cp "$O"/npu_fw/{MiniLoaderAll.bin,uboot.img,trust.img,boot.img} "$W/usr/share/npu_fw/"
        [ -f downloads/npu/LICENSE ] && install -D -m 0644 downloads/npu/LICENSE "$DOC/licenses/RK3399Pro_npu-LICENSE"
        [ -d "$O/npu-open/licenses" ] && cp "$O"/npu-open/licenses/* "$DOC/licenses/"
    else
        echo "WARNING: out/npu_fw/boot.img missing (scripts/build-npu-open.sh, then scripts/build-npu-boot.sh): the image gets no NPU firmware" >&2
    fi
fi
exit 0
