#!/usr/bin/env bash
# Write one file at a given LBA of the eMMC over USB, read it back, compare and
# (optionally) reset the board. For quick iterations on the loader or boot image.
#   scripts/flash-at.sh <LBA> <file> [--no-reset]
# e.g. U-Boot with the DDR blob:   scripts/flash-at.sh 64 out/u-boot-rockchip-ddrblob.bin
# Board in maskrom or loader mode (see docs/RECOVERY.md).
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
lsusb -d 2207:  >/dev/null 2>&1 || die "no Rockchip USB device: put the board into maskrom mode"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
size=$(stat -c %s "$FILE"); sectors=$(( (size + 511) / 512 ))
echo "$FILE ($(numfmt --to=iec "$size"), $sectors sectors) -> LBA $LBA"

load_loader
piece=$(mktemp -t flashpiece.XXXXXX); back=$(mktemp -t flashback.XXXXXX)
trap 'rm -f "$piece" "$back"' EXIT
off=0; per=$((16 * 2048))
while [ "$off" -lt "$sectors" ]; do
    n=$per; [ $((off + n)) -gt "$sectors" ] && n=$((sectors - off))
    dd if="$FILE" of="$piece" bs=512 skip="$off" count="$n" status=none
    ok=0
    for try in 1 2 3; do
        out=$($SUDO "$RKDT" wl $((LBA + off)) "$piece" 2>&1) || true
        echo "$out" | grep -qi fail || { ok=1; break; }
        echo "  attempt $try failed at sector $((LBA + off))"; sleep 2
    done
    [ "$ok" = 1 ] || die "write failed at sector $((LBA + off))"
    off=$((off + n))
done
echo "written $sectors sectors"
$SUDO "$RKDT" rl "$LBA" "$sectors" "$back" >/dev/null
cmp -n "$size" "$FILE" "$back" && echo "OK: LBA $LBA matches $FILE" || die "read-back differs: the board was NOT reset"
if [ "$RESET" = 1 ]; then
    echo "Resetting device..."; $SUDO "$RKDT" rd
    echo "Done. Disconnect USB-C, make sure J3 is open and power-cycle if the board does not start by itself."
fi
