#!/usr/bin/env bash
# Write the image to an SD card (or any removable disk), with safety checks.
#   scripts/flash-sd.sh /dev/sdX [image]         # asks for confirmation
#   scripts/flash-sd.sh -y /dev/sdX [image]      # no question
#   scripts/flash-sd.sh --dry-run /dev/sdX       # only show what would happen
set -euo pipefail
cd "$(dirname "$0")/.."

YES=0; DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) YES=1; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
DEV=${1:?usage: $0 [-y] [--dry-run] /dev/sdX [image]}
IMG=${2:-out/tinker-edge-r-debian13.img}

die() { echo "error: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "image not found: $IMG (build it first, see README)"
[ -b "$DEV" ] || die "$DEV is not a block device"
[ "$(lsblk -dno TYPE "$DEV")" = disk ] || die "$DEV is not a whole disk (use /dev/sdX, not a partition)"

name=$(basename "$DEV")

# never touch the disk the running system lives on (checked before anything else)
root_src=$(findmnt -n -o SOURCE / || true)
root_disk=$(lsblk -no PKNAME "$root_src" 2>/dev/null | head -1 || true)
[ "$name" != "$root_disk" ] || die "$DEV holds the running system"

size=$(lsblk -bdno SIZE "$DEV")
img_size=$(stat -c %s "$IMG")
[ "$size" -ge "$img_size" ] || die "$DEV ($size B) is smaller than the image ($img_size B)"

# anything mounted from it?
if lsblk -nlo MOUNTPOINTS "$DEV" | grep -q .; then
    die "$DEV has mounted partitions; unmount them first"
fi

removable=$(cat "/sys/block/$name/removable" 2>/dev/null || echo 0)
tran=$(lsblk -dno TRAN "$DEV" 2>/dev/null || true)
if [ "$removable" != 1 ] && [ "$tran" != usb ] && [ "${FORCE:-0}" != 1 ]; then
    die "$DEV is not removable/USB (set FORCE=1 to override)"
fi

echo "Image : $IMG ($(numfmt --to=iec "$img_size"))"
echo "Target: $DEV  $(lsblk -dno VENDOR,MODEL,SIZE "$DEV" | xargs)  (removable=$removable tran=${tran:-?})"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS "$DEV"
echo
echo "ALL DATA ON $DEV WILL BE ERASED."

if [ "$DRY" = 1 ]; then echo "(dry run: nothing written)"; exit 0; fi

if [ "$YES" != 1 ]; then
    read -r -p "Type the device name ($name) to continue: " ans
    [ "$ans" = "$name" ] || die "aborted"
fi

SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
$SUDO dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
sync

echo "verifying..."
if $SUDO cmp -n "$img_size" "$IMG" "$DEV"; then
    echo "OK: $DEV matches $IMG"
else
    die "verification failed"
fi
$SUDO partprobe "$DEV" 2>/dev/null || true
echo "Done. Insert the card into the Tinker Edge R, connect UART (115200 8N1) and power on."
