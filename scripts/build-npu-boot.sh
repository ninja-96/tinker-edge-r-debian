#!/usr/bin/env bash
# Make the boot image of the NPU die (the one that npu-load writes to it): Rockchip's firmware image with
#   1. the device tree adapted to this board (scripts/npu-fw-tinker.py: one fixed 0.85 V regulator instead of the EVB's I2C regulators),
#   2. our server, the TFLite VX delegate and TIM-VX added to the ramdisk (built by scripts/build-npu-open.sh),
#   3. the two init scripts changed so that our server takes the USB function and the stock rknn_server is only a fallback
#      (npu-open/ramdisk/modify_init.py).
# Kernel (Linux 4.4.185), the rest of the ramdisk and the Vivante libraries stay exactly as Rockchip made them.
# Needs dtc: run it in the builder container.
#   ./run.sh scripts/build-npu-boot.sh          -> out/npu_fw/{MiniLoaderAll.bin,uboot.img,trust.img,boot.img}
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${NPU_FW_SRC:-downloads/npu/npu_fw}
BIN=${NPU_OPEN_OUT:-out/npu-open}
O=out/npu_fw
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for f in MiniLoaderAll.bin uboot.img trust.img boot.img; do [ -f "$SRC/$f" ] || { echo "$SRC/$f missing: run scripts/fetch-npu.sh" >&2; exit 1; }; done
for f in npu_server libvx_delegate.so libtim-vx.so; do [ -f "$BIN/$f" ] || { echo "$BIN/$f missing: run scripts/build-npu-open.sh" >&2; exit 1; }; done

# an ASUS image (NPU_FW_SRC) already has the right regulators: the script then refuses it and the image is used as it is
python3 scripts/npu-fw-tinker.py "$SRC/boot.img" "$T/boot-dtb.img" || cp "$SRC/boot.img" "$T/boot-dtb.img"
R=npu-open/tools/npu_ramdisk.py
python3 $R "$T/boot-dtb.img" --extract "/etc/init.d/S50usbdevice=$T/S50.orig" --extract "/etc/init.d/S60NPU_init=$T/S60.orig" >/dev/null
python3 npu-open/ramdisk/modify_init.py S50usbdevice "$T/S50.orig" "$T/S50.new"
python3 npu-open/ramdisk/modify_init.py S60NPU_init "$T/S60.orig" "$T/S60.new"
mkdir -p "$O"
python3 $R "$T/boot-dtb.img" "$O/boot.img" \
    --add "/usr/bin/npu_server=$BIN/npu_server:755" \
    --add "/usr/lib/npu/libvx_delegate.so=$BIN/libvx_delegate.so:755" \
    --add "/usr/lib/libtim-vx.so=$BIN/libtim-vx.so:755" \
    --add "/etc/init.d/S50usbdevice=$T/S50.new:755" \
    --add "/etc/init.d/S60NPU_init=$T/S60.new:755"
cp "$SRC"/{MiniLoaderAll.bin,uboot.img,trust.img} "$O/"
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$O"
ls -l "$O"
