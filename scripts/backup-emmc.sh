#!/usr/bin/env bash
# Back up the whole eMMC of the board over USB (maskrom/loader mode).
# UNTESTED on hardware. Needs out/tools/ (./run.sh scripts/build-flash-tools.sh).
#
# Put the board in maskrom mode first (ASUS Quick Start Guide):
#   1. connect the USB-C data port to this PC
#   2. short the Recovery header J3 with a jumper
#   3. connect the power adapter (12-19 V DC)  -> `lsusb -d 2207:` shows the board
#
#   scripts/backup-emmc.sh [output.img]          (default: emmc-backup.img)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-emmc-backup.img}
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}
die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"

[ -x "$RKDT" ] && [ -f "$LOADER" ] || die "tools missing: ./run.sh scripts/build-flash-tools.sh"
[ ! -e "$OUT" ] || die "$OUT already exists (move it away first)"
if command -v lsusb >/dev/null && ! lsusb -d 2207: >/dev/null 2>&1; then
    die "no Rockchip USB device (VID 2207): see the steps at the top of this script"
fi
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo

devs=$($SUDO "$RKDT" ld 2>&1) || { echo "$devs"; die "rkdeveloptool failed (permissions? use sudo)"; }
echo "$devs"
echo "$devs" | grep -qi "DevNo=" || die "no device in maskrom/loader mode"
load_loader

info=$($SUDO "$RKDT" rfi)
echo "$info"
sectors=$(echo "$info" | awk '/Sectors/ {print $3}')
[ -n "$sectors" ] && [ "$sectors" -gt 0 ] || die "could not read the eMMC size"
echo "reading $sectors sectors ($(numfmt --to=iec $((sectors * 512)))) into $OUT ..."
$SUDO "$RKDT" rl 0 "$sectors" "$OUT"

size=$(stat -c %s "$OUT")
[ "$size" -eq $((sectors * 512)) ] || die "size mismatch: $size bytes, expected $((sectors * 512))"
sha256sum "$OUT" | tee "$OUT.sha256"
echo "Backup done. Keep $OUT and $OUT.sha256 safe."
