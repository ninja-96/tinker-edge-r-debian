#!/usr/bin/env bash
# Build the Debian 13 (trixie) arm64 root filesystem tarball: out/rootfs.tar
# Run inside the builder container:  ./run.sh scripts/build-rootfs.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=out
mkdir -p "$OUT"

# FREE=1: only Debian "main" (no contrib / non-free-firmware), no firmware blobs
FREE=${FREE:-0}
HOSTNAME_=${HOSTNAME_:-tinker-edge-r}
USER_=${USER_:-debian}
PASS_=${PASS_:-debian}

PACKAGES=(
    systemd-sysv systemd-resolved udev dbus kmod
    openssh-server sudo ca-certificates nftables
    iproute2 iputils-ping ethtool wpasupplicant iw
    usbutils pciutils less nano curl
    htop bash-completion fdisk util-linux-extra e2fsprogs python3 python3-numpy python3-libgpiod python3-smbus2 python3-spidev gpiod i2c-tools libusb-1.0-0
    systemd-timesyncd wireless-regdb rfkill
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav
    chromium
    plymouth
    locales alsa-utils bluez libgl1-mesa-dri libegl-mesa0 libgbm1 mesa-utils libdrm-tests mesa-opencl-icd clinfo
)
if [ "$FREE" = 1 ]; then
    COMPONENTS="main"
    OUTFILE="$OUT/rootfs-free.tar"
else
    COMPONENTS="main contrib non-free-firmware"
    OUTFILE="$OUT/rootfs.tar"
    PACKAGES+=(firmware-realtek firmware-brcm80211 firmware-linux-free)
fi

# Skip the (slow, network) build when nothing it depends on changed. FORCE=1 rebuilds.
# The key covers every effective (non-secret) parameter: a different host name, user or
# first-login-password-change policy is a different archive. A non-default password never
# uses the cache and does not publish a stamp: the built archive would carry that
# password and must not be served to a later default build.
PASS_DEFAULT=debian
PWC=${FORCE_PW_CHANGE:-1}
STAMP="rootfs $(sha256sum scripts/build-rootfs.sh | cut -c1-16) FREE=$FREE HOST=$HOSTNAME_ USER=$USER_ PWC=$PWC"
if [ "$PASS_" != "$PASS_DEFAULT" ]; then
    echo "rootfs: non-default password: building without the cache (and invalidating it)"
    rm -f "$OUTFILE.stamp"
elif [ "${FORCE:-0}" != 1 ] && [ -f "$OUTFILE" ] && [ -f "$OUTFILE.stamp" ] && [ "$(cat "$OUTFILE.stamp")" = "$STAMP" ]; then
    echo "rootfs: up to date (FORCE=1 rebuilds)"
    exit 0
fi
rm -f "$OUTFILE.stamp"   # published again only after the build succeeds

mmdebstrap --arch=arm64 --variant=minbase \
    --components="$COMPONENTS" \
    --include="$(IFS=,; echo "${PACKAGES[*]}")" \
    --customize-hook='
        set -e
        printf "deb https://deb.debian.org/debian-security trixie-security '"$COMPONENTS"'\ndeb https://deb.debian.org/debian trixie-updates '"$COMPONENTS"'\n" >> "$1/etc/apt/sources.list"
        echo "'"$HOSTNAME_"'" > "$1/etc/hostname"
        printf "127.0.0.1 localhost\n127.0.1.1 '"$HOSTNAME_"'\n" > "$1/etc/hosts"
        printf "LABEL=rootfs / ext4 defaults,noatime 0 1\n" > "$1/etc/fstab"
        mkdir -p "$1/etc/systemd/network"
        printf "[Match]\nName=e*\n\n[Network]\nDHCP=yes\n" > "$1/etc/systemd/network/20-wired.network"
        ln -sf /run/systemd/resolve/stub-resolv.conf "$1/etc/resolv.conf"
        chroot "$1" systemctl enable systemd-networkd systemd-resolved ssh nftables
        chroot "$1" useradd -m -s /bin/bash -G sudo,video,render,audio,dialout,plugdev '"$USER_"'
        echo "'"$USER_"':'"$PASS_"'" | chroot "$1" chpasswd
        [ "'"${FORCE_PW_CHANGE:-1}"'" = 1 ] && chroot "$1" chage -d 0 '"$USER_"'
        echo "en_US.UTF-8 UTF-8" > "$1/etc/locale.gen"
        chroot "$1" locale-gen
        echo "LANG=en_US.UTF-8" > "$1/etc/default/locale"
        rm -f "$1"/etc/ssh/ssh_host_*
        chroot "$1" apt-get clean
        rm -rf "$1/var/lib/apt/lists/"*' \
    trixie "$OUTFILE" https://deb.debian.org/debian

[ "$PASS_" = "$PASS_DEFAULT" ] && echo "$STAMP" > "$OUTFILE.stamp"
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUTFILE"
[ "$PASS_" = "$PASS_DEFAULT" ] && chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUTFILE.stamp"
ls -lh "$OUTFILE"
