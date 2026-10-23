#!/usr/bin/env bash
# Read the first part of the board's eMMC and classify it chunk by chunk:
# does it equal the written image, the original backup, zeros, or something else?
# Helps to see how far a write got. Board in maskrom/loader mode.
#   scripts/inspect-emmc.sh [megabytes=64] [image] [backup]
set -euo pipefail
cd "$(dirname "$0")/.."
MB=${1:-64}
IMG=${2:-out/tinker-edge-r-debian13.img}
BACKUP=${3:-emmc-backup.img}
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}
die() { echo "error: $*" >&2; exit 1; }
. "$(dirname "$0")/rockchip-usb.sh"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
[ -f "$IMG" ] && [ -f "$BACKUP" ] || die "need $IMG and $BACKUP"
lsusb -d 2207: >/dev/null 2>&1 || die "no Rockchip USB device"

load_loader
tmp=$(mktemp -p build inspect.XXXXXX.img)
trap 'rm -f "$tmp"' EXIT
$SUDO "$RKDT" rl 0 $((MB * 2048)) "$tmp"

python3 -I - "$tmp" "$IMG" "$BACKUP" "$MB" <<'PY'
import sys
emmc, img, bak, mb = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
C = 1 << 20
zero = bytes(C)
with open(emmc, "rb") as e, open(img, "rb") as i, open(bak, "rb") as b:
    rows = []
    for n in range(mb):
        de, di, db = e.read(C), i.read(C), b.read(C)
        if not de:
            break
        if de == di == db:   tag = "image=backup=eMMC"
        elif de == di:       tag = "= IMAGE"
        elif de == db:       tag = "= BACKUP (old content)"
        elif de == zero:     tag = "zeros"
        else:                tag = "other"
        rows.append(tag)
# compress runs
start = 0
for k in range(1, len(rows) + 1):
    if k == len(rows) or rows[k] != rows[start]:
        print(f"  MiB {start:3d}..{k-1:3d}  {rows[start]}")
        start = k
PY
