#!/usr/bin/env bash
# Write one file at a given LBA of the eMMC over USB, read it back, compare and
# (optionally) reset the board. For quick iterations on the loader or boot image.
#   scripts/flash-at.sh <LBA> <file> [--no-reset]
# e.g. U-Boot with the DDR blob:   scripts/flash-at.sh 64 out/u-boot-rockchip-ddrblob.bin
# Board in maskrom or loader mode (see docs/FLASHING.md).
set -euo pipefail
cd "$(dirname "$0")/.."
LBA=${1:?usage: $0 <LBA> <file> [--no-reset]}
FILE=${2:?usage: $0 <LBA> <file> [--no-reset]}
RESET=1; [ "${3:-}" = "--no-reset" ] && RESET=0
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}
die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"

[ -f "$FILE" ] || die "$FILE not found"
[ -x "$RKDT" ] && [ -f "$LOADER" ] || die "tools missing: ./run.sh scripts/build-flash-tools.sh"
[[ "$LBA" =~ ^(0|[1-9][0-9]{0,9}|0[xX][[:xdigit:]]{1,8})$ ]] || die "expected a decimal or hexadecimal LBA"
check_usb
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
size=$(stat -c %s "$FILE"); sectors=$(( (size + 511) / 512 ))
echo "$FILE ($(numfmt --to=iec "$size"), $sectors sectors) -> LBA $LBA"
read -r -p "Type the LBA ($LBA) to continue: " ans
[ "$ans" = "$LBA" ] || die "aborted: $FILE was not written"

load_loader
write_and_verify "$FILE" "$LBA" 0 $((16 * 2048)) 0
if [ "$RESET" = 1 ]; then
    echo "Resetting device..."; $SUDO "$RKDT" rd
    echo "Done. Disconnect USB-C, make sure J3 is open and power-cycle if the board does not start by itself."
fi
