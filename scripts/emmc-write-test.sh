#!/usr/bin/env bash
# Does the board's eMMC keep what we write beyond 32 MiB? Writes 1 MiB of a known
# pattern at several offsets through a given loader and reads it back.
# Only touches the OLD data area (>= 24 MiB); restore with flash-emmc.sh afterwards.
# Board in maskrom mode (a fresh power-on with J3 shorted, then J3 removed).
#   scripts/emmc-write-test.sh [loader.bin]   (default: out/tools/rk3399pro_loader.bin)
set -euo pipefail
cd "$(dirname "$0")/.."
RKDT=out/tools/rkdeveloptool
LOADER=${1:-out/tools/rk3399pro_loader.bin}
die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
[ -f "$LOADER" ] || die "no loader $LOADER"
lsusb -d 2207: >/dev/null 2>&1 || die "no Rockchip USB device"
echo "loader: $LOADER"
load_loader

pat=$(mktemp -p build wtest.XXXXXX); back=$(mktemp -p build wback.XXXXXX)
trap 'rm -f "$pat" "$back"' EXIT
python3 -I -c "
import os
open('$pat','wb').write(b''.join(bytes([(i*7+k)&255 for i in range(256)])*1 for k in range(4096)))"   # 1 MiB
for mb in 24 31 32 33 64 256 1024; do
    sector=$((mb * 2048))
    $SUDO "$RKDT" wl "$sector" "$pat" 2>&1 | tr '\r' '\n' | grep -i -E "fail|error" || true
    $SUDO "$RKDT" rl "$sector" 2048 "$back" >/dev/null 2>&1 || true
    if cmp -s "$pat" "$back"; then echo "  MiB $mb: write persisted"; else echo "  MiB $mb: LOST (read back differs)"; fi
done
