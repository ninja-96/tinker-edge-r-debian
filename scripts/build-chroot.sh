# Shared setup for the emulated arm64 build scripts. W is their scratch root.
#
# aarch64 user-mode emulation (qemu) works through a global binfmt_misc registration.
# The builder image ships qemu-user and the registration spec (qemu-user-binfmt), but
# has no init to register it. binfmt entries live in the host kernel and persist for
# its lifetime, so this only (re)registers when the host has been rebooted. Needs the
# container to run --privileged (run.sh always does).
ensure_binfmt() {
    [ "$(uname -m)" = aarch64 ] && return 0
    local d=/proc/sys/fs/binfmt_misc
    if grep -qx enabled "$d/qemu-aarch64" 2>/dev/null && grep -qx enabled "$d/status"; then return 0; fi
    if ! mountpoint -q "$d"; then
        mount -t binfmt_misc binfmt_misc "$d" || {
            echo "ensure_binfmt: cannot mount binfmt_misc (the container needs --privileged; see run.sh)" >&2
            return 1
        }
    fi
    if ! grep -qx enabled "$d/status"; then echo 1 > "$d/status" || return 1; fi
    if [ -e "$d/qemu-aarch64" ]; then echo 1 > "$d/qemu-aarch64"; return; fi
    cat /usr/lib/binfmt.d/qemu-aarch64.conf > "$d/register"
}

cleanup() {
    local m
    for m in dev/pts dev sys proc; do
        if mountpoint -q "$W/$m"; then umount "$W/$m" || return 1; fi
    done
}

prepare_chroot() {
    cleanup
    ensure_binfmt
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
