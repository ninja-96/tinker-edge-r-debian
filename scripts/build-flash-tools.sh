#!/usr/bin/env bash
# Build the USB flashing tools for the host (x86_64):
#   out/tools/rkdeveloptool          open-source maskrom/loader flasher
#   out/tools/rk3399pro_loader.bin   usbplug+ddr+miniloader merged with boot_merger
# Run inside the builder container:  ./run.sh scripts/build-flash-tools.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$PWD/out/tools
mkdir -p "$OUT" build

# --- loader: Rockchip boot_merger (x86_64 binary in rkbin) + RK3399Pro ini
( cd downloads/rkbin
  ./tools/boot_merger RKBOOT/RK3399PROMINIALL.ini
  mv -f rk3399pro_loader_v*.bin "$OUT/rk3399pro_loader.bin" )

# --- rkdeveloptool: out-of-tree copy so downloads/ stays clean
rm -rf build/rkdeveloptool
cp -a downloads/rkdeveloptool build/rkdeveloptool
cd build/rkdeveloptool
autoreconf -i >/dev/null
./configure CXXFLAGS="-O2 -Wno-error -fpermissive" >/dev/null
make -j"$(nproc)" >/dev/null
install -m 0755 rkdeveloptool "$OUT/rkdeveloptool"
install -m 0644 99-rk-rockusb.rules "$OUT/99-rk-rockusb.rules"
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT"
ls -l "$OUT"
