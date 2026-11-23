#!/usr/bin/env bash
# Build the v4l2_request VA-API driver (libva-v4l2-request, dongioia fork; MIT/LGPL-2.1)
# for arm64. It exposes the mainline rkvdec V4L2 stateless decoder (the -browser device
# tree) to Chromium, which decodes video through VA-API (its own V4L2 decoder was
# removed upstream). The build runs inside a scratch arm64 root filesystem (qemu user
# emulation), so the kernel UAPI headers are the same ones the board ships with;
# only the driver library is kept.
# Output: out/v4l2-request/  (copied into the rootfs by stage-rootfs.sh)
# Run inside the builder container:  ./run.sh scripts/build-v4l2-request.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
. ./sources.lock
[ -f out/rootfs.tar ] || { echo "run build-rootfs.sh first" >&2; exit 1; }

D=downloads/libva-v4l2-request
g() { git -c safe.directory="*" -C "$D" "$@"; }
if [ ! -d "$D/.git" ]; then
    git init -q "$D"
    g remote add origin "$V4L2_REQUEST_REPO"
fi
if [ "$(g rev-parse HEAD 2>/dev/null || true)" != "$V4L2_REQUEST_REV" ]; then
    g fetch -q origin "$V4L2_REQUEST_BRANCH"
    g checkout -q --force "$V4L2_REQUEST_REV"
fi
echo "libva-v4l2-request @ $(g rev-parse --short HEAD)"

# Skip the (slow, emulated) build when nothing it depends on changed. FORCE=1 rebuilds.
STAMP="$V4L2_REQUEST_REV $(sha256sum scripts/build-v4l2-request.sh | cut -c1-16)"
if [ "${FORCE:-0}" != 1 ] && [ -f out/v4l2-request/.stamp ] && [ "$(cat out/v4l2-request/.stamp)" = "$STAMP" ]; then
    echo "v4l2-request: up to date (FORCE=1 rebuilds)"
    exit 0
fi

W=$ROOT/build/v4l2-chroot
cleanup() {
    for m in dev/pts dev sys proc; do umount "$W/$m" 2>/dev/null || true; done
}
trap cleanup EXIT
cleanup
rm -rf "$W" out/v4l2-request; mkdir -p "$W" out/v4l2-request
tar -xf out/rootfs.tar -C "$W"
mkdir -p "$W/tmp/v4l2-request"
tar -C "$D" --exclude=.git -cf - . | tar -C "$W/tmp/v4l2-request" -xf -
rm -f "$W/etc/resolv.conf"; cp /etc/resolv.conf "$W/etc/resolv.conf"
mount -t proc proc "$W/proc"; mount --bind /sys "$W/sys"
mount --bind /dev "$W/dev"; mount --bind /dev/pts "$W/dev/pts"

chroot "$W" /bin/sh -e -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential meson ninja-build pkg-config \
        libva-dev libdrm-dev
    cd /tmp/v4l2-request
    meson setup build --prefix=/usr --buildtype=release
    ninja -C build
    DESTDIR=/tmp/v4l2-out ninja -C build install
'
cleanup
cp -a "$W/tmp/v4l2-out/usr" out/v4l2-request/
rm -rf "$W"
echo "$STAMP" > out/v4l2-request/.stamp
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" out/v4l2-request
find out/v4l2-request -type f \( -name '*.so*' -o -name '*.stamp' \) | sed 's#^#  #'
