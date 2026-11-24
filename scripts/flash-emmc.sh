#!/usr/bin/env bash
# Write the image to the board's eMMC over USB (Rockchip maskrom/loader mode).
# UNTESTED on hardware. Needs out/tools/rkdeveloptool and
# out/tools/rk3399_loader.bin (run ./run.sh scripts/build-flash-tools.sh).
#
#   scripts/flash-emmc.sh --backup emmc-backup.img [image]
#   scripts/flash-emmc.sh --dry-run --backup ...        # show the plan only
#   --start-mb N      resume at MiB N (after a failed run)
#   --no-reset        do not reset the board at the end
#
# Loader: out/tools/rk3399_loader.bin (the plain RK3399 one). The RK3399Pro loader
# silently dropped writes beyond 32 MiB on a Tinker Edge R; override with LOADER=...
#
# The image is written in 16 MiB pieces with retries and read back afterwards;
# the board is reset only if the read-back matches.
#
# Maskrom mode (ASUS Quick Start Guide): connect the USB-C data port to this PC,
# short the Recovery header J3, then connect the 12-19 V power adapter.
# `lsusb -d 2207:` must show the board. Remove the jumper before the next reboot.
set -euo pipefail
cd "$(dirname "$0")/.."

YES=0; DRY=0; BACKUP=""; SKIP=0; START_MB=0; RESET=1
CHUNK_MB=${CHUNK_MB:-16}
while [ $# -gt 0 ]; do
    case "$1" in
        --backup) BACKUP=${2:?--backup needs a file}; shift 2 ;;
        --skip-backup-check) SKIP=1; shift ;;
        --start-mb) START_MB=${2:?--start-mb needs a number}; shift 2 ;;
        --no-reset) RESET=0; shift ;;
        -y|--yes) YES=1; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
IMG=${1:-out/tinker-edge-r-debian13.img}
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}

die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"

[ -f "$IMG" ] || die "image not found: $IMG"
[ -x "$RKDT" ] || die "$RKDT missing: run ./run.sh scripts/build-flash-tools.sh"
[ -f "$LOADER" ] || die "$LOADER missing: run ./run.sh scripts/build-flash-tools.sh"

if [ "$SKIP" != 1 ]; then
    [ -n "$BACKUP" ] || die "give --backup <file> (a copy of the stock eMMC), or --skip-backup-check"
    [ -s "$BACKUP" ] || die "backup file $BACKUP is missing or empty"
    [ "$(stat -c %s "$BACKUP")" -gt $((100*1024*1024)) ] || die "backup file $BACKUP looks too small to be an eMMC image"
fi

[[ "$START_MB" =~ ^(0|[1-9][0-9]{0,6})$ && "$CHUNK_MB" =~ ^[1-9][0-9]{0,6}$ ]] || die "invalid start or chunk size"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
size=$(stat -c %s "$IMG")
total_mb=$(( (size + 1048575) / 1048576 ))

echo "Image  : $IMG ($(numfmt --to=iec "$size"), $total_mb MiB)"
echo "Backup : ${BACKUP:-<check skipped>}"
echo "Plan   : load loader -> write in ${CHUNK_MB} MiB pieces from MiB $START_MB -> read back and compare -> reset"
if [ "$DRY" = 1 ]; then echo "(dry run: nothing executed)"; exit 0; fi

if command -v lsusb >/dev/null && ! lsusb -d 2207: >/dev/null 2>&1; then
    die "no Rockchip USB device (VID 2207) on the bus: put the board into maskrom mode and connect the USB-C data port (see docs/FLASHING.md)"
fi
echo "Devices:"
if ! devs=$($SUDO "$RKDT" ld 2>&1); then
    echo "$devs"
    die "rkdeveloptool failed (permissions? install out/tools/99-rk-rockusb.rules or run with sudo)"
fi
echo "$devs"
echo "$devs" | grep -qi "DevNo=" || die "rkdeveloptool sees no device in maskrom/loader mode (see docs/FLASHING.md)"
[ "$(echo "$devs" | grep -ci "DevNo=")" = 1 ] || die "more than one Rockchip device: unplug the others"

if [ "$YES" != 1 ]; then
    echo
    echo "THIS ERASES THE eMMC OF THE CONNECTED BOARD."
    read -r -p "Type ERASE-EMMC to continue: " ans
    [ "$ans" = "ERASE-EMMC" ] || die "aborted"
fi

load_loader

write_and_verify "$IMG" 0 $((START_MB * 2048)) $((CHUNK_MB * 2048)) 1
if [ "$RESET" = 1 ]; then
    echo "Resetting device..."
    $SUDO "$RKDT" rd
    echo "Done. Remove the SD card if present and J3, power-cycle the board and watch the UART."
else
    echo "Done (board not reset)."
fi
