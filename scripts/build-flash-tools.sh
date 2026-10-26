#!/usr/bin/env bash
# Build the PC-side flashing tools into out/tools/:
#   rkdeveloptool       from third_party/rkdeveloptool (GPL, copied unmodified) plus patches/rkdeveloptool/
#   rk3399_loader.bin   the USB loader, copied from blobs/rockchip/
#   99-rk-rockusb.rules optional udev rule for non-root USB access
# Nothing is downloaded. Run inside the builder container:
#   ./run.sh scripts/build-flash-tools.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$PWD/out/tools
mkdir -p "$OUT" build

( cd blobs/rockchip && sha256sum -c SHA256SUMS >/dev/null ) || { echo "blob checksum mismatch" >&2; exit 1; }
install -m 0644 blobs/rockchip/rk3399_loader.bin "$OUT/rk3399_loader.bin"

rm -rf build/rkdeveloptool
cp -a third_party/rkdeveloptool build/rkdeveloptool
cd build/rkdeveloptool
for p in ../../patches/rkdeveloptool/*.patch; do patch -p1 -s < "$p"; done
autoreconf -i >/dev/null 2>&1
./configure CXXFLAGS="-O2 -Wno-error -fpermissive" >/dev/null
make -j"$(nproc)" >/dev/null
install -m 0755 rkdeveloptool "$OUT/rkdeveloptool"
install -m 0644 99-rk-rockusb.rules "$OUT/99-rk-rockusb.rules"
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT"
ls -l "$OUT"
