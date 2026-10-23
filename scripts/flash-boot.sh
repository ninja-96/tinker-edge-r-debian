#!/usr/bin/env bash
# Rewrite only the "boot" partition (out/boot.img at LBA 40960, partition 4 of
# the image layout) over USB, read it back, compare, then reset the board.
# Much faster than a full flash after a change of the kernel/DTB/boot image.
# Board in maskrom or loader mode (see docs/RECOVERY.md).
#   scripts/flash-boot.sh [boot.img]       (--no-reset to keep the board in loader mode)
set -euo pipefail
cd "$(dirname "$0")/.."
RESET=1; [ "${1:-}" = "--no-reset" ] && { RESET=0; shift; }
BOOT=${1:-out/boot.img}
BOOT_LBA=40960                       # partition 4 "boot" in scripts/build-image.sh
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}
die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"

[ -f "$BOOT" ] || die "$BOOT not found (build it: ./run.sh scripts/build-image.sh)"
[ -x "$RKDT" ] && [ -f "$LOADER" ] || die "tools missing: ./run.sh scripts/build-flash-tools.sh"
lsusb -d 2207: >/dev/null 2>&1 || die "no Rockchip USB device: put the board into maskrom mode"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
size=$(stat -c %s "$BOOT"); sectors=$(( (size + 511) / 512 ))
echo "boot image: $BOOT ($(numfmt --to=iec "$size"), $sectors sectors) -> LBA $BOOT_LBA"

load_loader
back=$(mktemp -p build bootback.XXXXXX); trap 'rm -f "$back"' EXIT

# write in 16 MiB pieces with retries (same reason as flash-emmc.sh)
piece=$(mktemp -p build bootpiece.XXXXXX); trap 'rm -f "$back" "$piece"' EXIT
off=0; per=$((16 * 2048))
while [ "$off" -lt "$sectors" ]; do
    n=$per; [ $((off + n)) -gt "$sectors" ] && n=$((sectors - off))
    dd if="$BOOT" of="$piece" bs=512 skip="$off" count="$n" status=none
    ok=0
    for try in 1 2 3; do
        out=$($SUDO "$RKDT" wl $((BOOT_LBA + off)) "$piece" 2>&1) || true
        echo "$out" | grep -qi fail || { ok=1; break; }
        echo "  attempt $try failed at sector $((BOOT_LBA + off))"; sleep 2
    done
    [ "$ok" = 1 ] || die "write failed at sector $((BOOT_LBA + off))"
    off=$((off + n))
done
echo "written $sectors sectors"

$SUDO "$RKDT" rl "$BOOT_LBA" "$sectors" "$back" >/dev/null
if cmp -n "$size" "$BOOT" "$back"; then
    echo "OK: boot partition matches $BOOT"
else
    die "read-back differs: the board was NOT reset"
fi
if [ "$RESET" = 1 ]; then
    echo "Resetting device..."
    $SUDO "$RKDT" rd
    echo "Done. Remove J3 (if still shorted), disconnect USB-C and power-cycle if the board does not start by itself."
fi
