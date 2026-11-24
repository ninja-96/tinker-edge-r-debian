# Shared setup for the emulated arm64 build scripts. W is their scratch root.
cleanup() {
    local m
    for m in dev/pts dev sys proc; do
        if mountpoint -q "$W/$m"; then umount "$W/$m" || return 1; fi
    done
}

prepare_chroot() {
    cleanup
    rm -rf "$W"; mkdir -p "$W"
    tar -xf out/rootfs.tar -C "$W"
    rm -f "$W/etc/resolv.conf"; cp /etc/resolv.conf "$W/etc/resolv.conf"
    mount -t proc proc "$W/proc"
    mount --bind /sys "$W/sys"
    mount --bind /dev "$W/dev"
    mount --bind /dev/pts "$W/dev/pts"
}

install_build_deps() {
    chroot "$W" env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    chroot "$W" env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends build-essential "$@"
}
