#!/usr/bin/env bash
# Read the beginning of the board's eMMC back over USB and compare it with the
# image that was written. UNTESTED on hardware.
# Board in maskrom mode (USB-C to the PC, J3 shorted at power-on, then removed).
#   scripts/verify-emmc.sh [image]       (default: out/tinker-edge-r-debian13.img)
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=${1:-out/tinker-edge-r-debian13.img}
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}
die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"

[ -f "$IMG" ] || die "image not found: $IMG"
[ -x "$RKDT" ] && [ -f "$LOADER" ] || die "tools missing: ./run.sh scripts/build-flash-tools.sh"
lsusb -d 2207: >/dev/null 2>&1 || die "no Rockchip USB device: put the board into maskrom mode"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo

load_loader
size=$(stat -c %s "$IMG")
sectors=$(( (size + 511) / 512 ))
tmp=$(mktemp -t --suffix=.img verify.XXXXXX)
trap 'rm -f "$tmp"' EXIT
echo "reading $sectors sectors back from the eMMC..."
$SUDO "$RKDT" rl 0 "$sectors" "$tmp"

python3 -I - "$tmp" "$IMG" <<'PY'
import sys
emmc, img = sys.argv[1], sys.argv[2]
C = 1 << 20
bad = []
with open(emmc, "rb") as e, open(img, "rb") as i:
    n = 0
    while True:
        di = i.read(C)
        if not di:
            break
        de = e.read(len(di))           # the last piece of the image may be shorter than 1 MiB
        if de != di:
            bad.append(n)
        n += 1
total = n
# compress to ranges
ranges = []
for m in bad:
    if ranges and ranges[-1][1] == m - 1:
        ranges[-1][1] = m
    else:
        ranges.append([m, m])
print(f"compared {total} MiB; differing MiB ranges: " +
      (", ".join(f"{a}..{b}" if a != b else f"{a}" for a, b in ranges) if ranges else "none"))
# MiB 3 (sector 7168 and up) is rewritten by the board itself (vendor storage), ignore it
real = [m for m in bad if m != 3]
if not real:
    print("OK: the eMMC contains the image (MiB 3, the board's own area, is ignored)")
    sys.exit(0)
print(f"MISMATCH: {len(real)} MiB differ; first at MiB {real[0]}, last at MiB {real[-1]}")
sys.exit(1)
PY
