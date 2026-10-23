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

SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
size=$(stat -c %s "$IMG")
total_mb=$(( (size + 1048575) / 1048576 ))

echo "Image  : $IMG ($(numfmt --to=iec "$size"), $total_mb MiB)"
echo "Backup : ${BACKUP:-<check skipped>}"
echo "Plan   : load loader -> write in ${CHUNK_MB} MiB pieces from MiB $START_MB -> read back and compare -> reset"
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

load_loader

# --- write in pieces
tmp=$(mktemp -t flashpiece.XXXXXX)
trap 'rm -f "$tmp"' EXIT
mb=$START_MB
while [ "$mb" -lt "$total_mb" ]; do
    n=$CHUNK_MB; [ $((mb + n)) -gt "$total_mb" ] && n=$((total_mb - mb))
    dd if="$IMG" of="$tmp" bs=1M skip="$mb" count="$n" status=none
    sector=$((mb * 2048))
    ok=0
    for try in 1 2 3; do
        out=$($SUDO "$RKDT" wl "$sector" "$tmp" 2>&1) || true
        if ! echo "$out" | grep -qi "fail"; then ok=1; break; fi
        echo "  piece at MiB $mb (sector $sector), attempt $try failed:"
        echo "$out" | tr '\r' '\n' | grep -v '^$' | tail -3 | sed 's/^/    /'
        sleep 2
    done
    if [ "$ok" != 1 ]; then
        die "write failed at MiB $mb (sector $sector) after 3 attempts. Resume with: --start-mb $mb (the board is still in loader mode)"
    fi
    printf '\rwritten %d / %d MiB' $((mb + n)) "$total_mb"
    mb=$((mb + n))
done
echo

# --- read back and compare (MiB 3 is rewritten by the board itself)
echo "Reading back to verify..."
back=$(mktemp -t flashback.XXXXXX)
trap 'rm -f "$tmp" "$back"' EXIT
$SUDO "$RKDT" rl 0 $((total_mb * 2048)) "$back" >/dev/null
python3 -I - "$back" "$IMG" <<'PY' || die "read-back does not match the image: the board was NOT reset. Re-run with --start-mb to repair, or send this output"
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
        if e.read(len(di)) != di:      # the last piece of the image may be shorter than 1 MiB
            bad.append(n)
        n += 1
real = [m for m in bad if m != 3]
print(f"compared {n} MiB; differing outside the board's own area: {len(real)}")
if real:
    print(f"first differing MiB: {real[0]}, last: {real[-1]}")
    sys.exit(1)
print("OK: the eMMC contains the image")
PY

if [ "$RESET" = 1 ]; then
    echo "Resetting device..."
    $SUDO "$RKDT" rd
    echo "Done. Remove the SD card if present and J3, power-cycle the board and watch the UART."
else
    echo "Done (board not reset)."
fi
