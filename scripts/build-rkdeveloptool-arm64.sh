#!/usr/bin/env bash
# Build rkdeveloptool (GPL-2.0+, third_party/ plus patches/rkdeveloptool/) for arm64 inside a scratch
# copy of the arm64 root filesystem (qemu user emulation), so that the board can load the NPU
# firmware (npu-load) without Rockchip's closed upgrade_tool. Needs out/rootfs.tar.
# Output: out/rkdeveloptool-arm64/usr/bin/rkdeveloptool  (copied into the rootfs by stage-rootfs.sh)
# Run inside the builder container:  ./run.sh scripts/build-rkdeveloptool-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
[ -f out/rootfs.tar ] || { echo "run build-rootfs.sh first" >&2; exit 1; }

STAMP="$(cat third_party/rkdeveloptool/*.cpp third_party/rkdeveloptool/*.h patches/rkdeveloptool/*.patch scripts/build-rkdeveloptool-arm64.sh | sha256sum | cut -c1-32)"
if [ "${FORCE:-0}" != 1 ] && [ -f out/rkdeveloptool-arm64/.stamp ] && [ "$(cat out/rkdeveloptool-arm64/.stamp)" = "$STAMP" ]; then
    echo "rkdeveloptool arm64: up to date (FORCE=1 rebuilds)"; exit 0
fi

W=$ROOT/build/rkd-chroot
cleanup() { for m in dev/pts dev sys proc; do umount "$W/$m" 2>/dev/null || true; done; }
trap cleanup EXIT
cleanup
rm -rf "$W" out/rkdeveloptool-arm64; mkdir -p "$W" out/rkdeveloptool-arm64
tar -xf out/rootfs.tar -C "$W"
mkdir -p "$W/tmp/rkd" "$W/tmp/patches"
cp -a third_party/rkdeveloptool/. "$W/tmp/rkd/"
cp patches/rkdeveloptool/*.patch "$W/tmp/patches/"
rm -f "$W/etc/resolv.conf"; cp /etc/resolv.conf "$W/etc/resolv.conf"
mount -t proc proc "$W/proc"; mount --bind /sys "$W/sys"
mount --bind /dev "$W/dev"; mount --bind /dev/pts "$W/dev/pts"

chroot "$W" /bin/sh -e -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential autoconf automake libtool pkg-config \
        libusb-1.0-0-dev libudev-dev patch
    cd /tmp/rkd
    for p in /tmp/patches/*.patch; do patch -p1 -s < "$p"; done
    autoreconf -i >/dev/null 2>&1
    ./configure CXXFLAGS="-O3 -mcpu=cortex-a72.cortex-a53 -Wno-error -fpermissive" >/dev/null
    make -j"$(nproc)" >/dev/null
    install -D -m 0755 rkdeveloptool /tmp/out/usr/bin/rkdeveloptool
'
cleanup
cp -a "$W/tmp/out/usr" out/rkdeveloptool-arm64/usr
rm -rf "$W"
echo "$STAMP" > out/rkdeveloptool-arm64/.stamp
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" out/rkdeveloptool-arm64
file out/rkdeveloptool-arm64/usr/bin/rkdeveloptool
