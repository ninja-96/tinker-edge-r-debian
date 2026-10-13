#!/usr/bin/env bash
# Build the Debian 13 (trixie) arm64 root filesystem tarball: out/rootfs.tar
# Run inside the builder container:  ./run.sh scripts/build-rootfs.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=out
mkdir -p "$OUT"

HOSTNAME_=${HOSTNAME_:-tinker-edge-r}
USER_=${USER_:-debian}
PASS_=${PASS_:-debian}

PACKAGES=(
    systemd-sysv systemd-resolved udev dbus kmod
    openssh-server sudo ca-certificates
    iproute2 iputils-ping ethtool wpasupplicant iw
    firmware-brcm80211 firmware-linux-free
    usbutils pciutils less nano curl
    fdisk python3-minimal libusb-1.0-0
    systemd-timesyncd firmware-realtek wireless-regdb rfkill
)

mmdebstrap --arch=arm64 --variant=minbase \
    --components="main contrib non-free-firmware" \
    --include="$(IFS=,; echo "${PACKAGES[*]}")" \
    --customize-hook='
        set -e
        echo "'"$HOSTNAME_"'" > "$1/etc/hostname"
        printf "127.0.0.1 localhost\n127.0.1.1 '"$HOSTNAME_"'\n" > "$1/etc/hosts"
        printf "LABEL=rootfs / ext4 defaults,noatime 0 1\n" > "$1/etc/fstab"
        mkdir -p "$1/etc/systemd/network"
        printf "[Match]\nName=e*\n\n[Network]\nDHCP=yes\n" > "$1/etc/systemd/network/20-wired.network"
        ln -sf /run/systemd/resolve/stub-resolv.conf "$1/etc/resolv.conf"
        chroot "$1" systemctl enable systemd-networkd systemd-resolved ssh
        chroot "$1" useradd -m -s /bin/bash -G sudo,video,audio,dialout,plugdev '"$USER_"'
        echo "'"$USER_"':'"$PASS_"'" | chroot "$1" chpasswd
        rm -f "$1"/etc/ssh/ssh_host_*
        chroot "$1" apt-get clean
        rm -rf "$1/var/lib/apt/lists/"*' \
    trixie "$OUT/rootfs.tar" https://deb.debian.org/debian

chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT/rootfs.tar"
ls -lh "$OUT/rootfs.tar"
