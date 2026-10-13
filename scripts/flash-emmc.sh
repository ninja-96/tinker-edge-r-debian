#!/usr/bin/env bash
# Write the image to the board's eMMC over USB (Rockchip maskrom/loader mode).
# UNTESTED on hardware. Needs: out/tools/rkdeveloptool and
# out/tools/rk3399pro_loader.bin (run ./run.sh scripts/build-flash-tools.sh).
#
#   scripts/flash-emmc.sh --backup emmc-backup.img.gz [image]
#   scripts/flash-emmc.sh --dry-run --backup ...        # show the plan only
#
# How to put the board into maskrom mode (button/jumper) is NOT verified for the
# Tinker Edge R yet: see docs/RECOVERY.md.
set -euo pipefail
cd "$(dirname "$0")/.."

YES=0; DRY=0; BACKUP=""; SKIP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --backup) BACKUP=${2:?--backup needs a file}; shift 2 ;;
        --skip-backup-check) SKIP=1; shift ;;
        -y|--yes) YES=1; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
IMG=${1:-out/tinker-edge-r-debian13.img}
RKDT=out/tools/rkdeveloptool
LOADER=out/tools/rk3399pro_loader.bin

die() { echo "error: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "image not found: $IMG"
[ -x "$RKDT" ] || die "$RKDT missing: run ./run.sh scripts/build-flash-tools.sh"
[ -f "$LOADER" ] || die "$LOADER missing: run ./run.sh scripts/build-flash-tools.sh"

if [ "$SKIP" != 1 ]; then
    [ -n "$BACKUP" ] || die "give --backup <file> (a copy of the stock eMMC), or --skip-backup-check"
    [ -s "$BACKUP" ] || die "backup file $BACKUP is missing or empty"
    [ "$(stat -c %s "$BACKUP")" -gt $((100*1024*1024)) ] || die "backup file $BACKUP looks too small to be an eMMC image"
fi

SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo

echo "Image  : $IMG ($(numfmt --to=iec "$(stat -c %s "$IMG")"))"
echo "Backup : ${BACKUP:-<check skipped>}"
echo "Plan   : rkdeveloptool ld -> db <loader> -> wl 0 <image> -> rd"
if [ "$DRY" = 1 ]; then echo "(dry run: nothing executed)"; exit 0; fi

if command -v lsusb >/dev/null && ! lsusb -d 2207: >/dev/null 2>&1; then
    die "no Rockchip USB device (VID 2207) on the bus: put the board into maskrom mode and connect the USB-C data port (see docs/RECOVERY.md)"
fi

echo "Devices:"
if ! devs=$($SUDO "$RKDT" ld 2>&1); then
    echo "$devs"
    die "rkdeveloptool failed (permissions? install out/tools/99-rk-rockusb.rules or run with sudo)"
fi
echo "$devs"
echo "$devs" | grep -qi "DevNo=" || die "rkdeveloptool sees no device in maskrom/loader mode (see docs/RECOVERY.md)"
[ "$(echo "$devs" | grep -ci "DevNo=")" = 1 ] || die "more than one Rockchip device: unplug the others"

if [ "$YES" != 1 ]; then
    echo
    echo "THIS ERASES THE eMMC OF THE CONNECTED BOARD."
    read -r -p "Type ERASE-EMMC to continue: " ans
    [ "$ans" = "ERASE-EMMC" ] || die "aborted"
fi

if echo "$devs" | grep -qi maskrom; then
    echo "Maskrom: downloading loader..."
    $SUDO "$RKDT" db "$LOADER"
    sleep 2
fi
echo "Writing image to eMMC (LBA 0)..."
$SUDO "$RKDT" wl 0 "$IMG"
echo "Resetting device..."
$SUDO "$RKDT" rd
echo "Done. Remove the SD card if present, power-cycle the board and watch the UART."
