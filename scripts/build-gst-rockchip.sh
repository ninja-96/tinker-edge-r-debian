#!/usr/bin/env bash
# Build the Rockchip GStreamer plugins (rockchipmpp: mpph264enc, mppvideodec, mppjpegenc,
# mppjpegdec, mppvp8enc; LGPL-2.1) for arm64. They sit on top of librockchip_mpp.
# The build runs inside a scratch copy of the arm64 root filesystem (qemu user emulation),
# so the real GStreamer development packages of Debian are used; only the plugin libraries
# are kept.  Needs out/rootfs.tar and out/mpp (run build-rootfs.sh and build-mpp.sh first).
# Output: out/gst-rockchip/usr/  (copied into the rootfs by stage-rootfs.sh)
# Run inside the builder container:  ./run.sh scripts/build-gst-rockchip.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
. ./sources.lock
[ -f out/rootfs.tar ] && [ -d out/mpp/usr ] || { echo "run build-rootfs.sh and build-mpp.sh first" >&2; exit 1; }

D=downloads/gst-rockchip
g() { git -c safe.directory="*" -C "$D" "$@"; }
if [ ! -d "$D/.git" ]; then
    git init -q "$D"
    g remote add origin "$GST_ROCKCHIP_REPO"
fi
if [ "$(g rev-parse HEAD 2>/dev/null || true)" != "$GST_ROCKCHIP_REV" ]; then
    g fetch -q --depth 1 origin "$GST_ROCKCHIP_REV"
    g checkout -q --force FETCH_HEAD
fi
echo "gst-rockchip @ $(g rev-parse --short HEAD)"

# Skip the (slow, emulated) build when nothing it depends on changed. FORCE=1 rebuilds.
STAMP="$GST_ROCKCHIP_REV $(sha256sum out/mpp/usr/lib/librockchip_mpp.so.0 | cut -c1-16) $(sha256sum scripts/build-gst-rockchip.sh | cut -c1-16)"
if [ "${FORCE:-0}" != 1 ] && [ -f out/gst-rockchip/.stamp ] && [ "$(cat out/gst-rockchip/.stamp)" = "$STAMP" ]; then
    echo "gst-rockchip: up to date (FORCE=1 rebuilds)"
    exit 0
fi

W=$ROOT/build/gst-chroot
cleanup() {
    for m in dev/pts dev sys proc; do umount "$W/$m" 2>/dev/null || true; done
}
trap cleanup EXIT
cleanup
rm -rf "$W" out/gst-rockchip; mkdir -p "$W" out/gst-rockchip
tar -xf out/rootfs.tar -C "$W"
cp -a out/mpp/usr/. "$W/usr/"
mkdir -p "$W/tmp/gst-rockchip"
tar -C "$D" --exclude=.git -cf - . | tar -C "$W/tmp/gst-rockchip" -xf -
rm -f "$W/etc/resolv.conf"; cp /etc/resolv.conf "$W/etc/resolv.conf"
mount -t proc proc "$W/proc"; mount --bind /sys "$W/sys"
mount --bind /dev "$W/dev"; mount --bind /dev/pts "$W/dev/pts"

chroot "$W" /bin/sh -e -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential meson ninja-build pkg-config \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev libdrm-dev
    cd /tmp/gst-rockchip
    meson setup build --prefix=/usr --buildtype=release
    ninja -C build
    DESTDIR=/tmp/gst-out ninja -C build install
'
cleanup
cp -a "$W/tmp/gst-out/usr" out/gst-rockchip/usr
rm -rf "$W"
echo "$STAMP" > out/gst-rockchip/.stamp
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" out/gst-rockchip
find out/gst-rockchip -name '*.so' | sed 's#^#  #'
