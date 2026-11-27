#!/usr/bin/env bash
# Install only U-Boot on eMMC; the operating system lives on SD.
#   scripts/flash-emmc-loader.sh --backup emmc-backup.img [loader-only.img]
# The existing eMMC partition table is removed; this is not a secure erase.
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "error: $*" >&2; exit 1; }
[ "${1:-}" = --backup ] || die "usage: $0 --backup <whole-emmc-backup.img> [loader-only.img]"
BACKUP=${2:?backup required}
FILE=${3:-out/tinker-edge-r-uboot-emmc.img}
RKDT=out/tools/rkdeveloptool
LOADER=${LOADER:-out/tools/rk3399_loader.bin}
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
. scripts/rockchip-usb.sh

[ -f "$FILE" ] && [ -f "$BACKUP" ] || die "need loader and whole-eMMC backup"
[ -x "$RKDT" ] && [ -f "$LOADER" ] || die "tools missing: ./run.sh scripts/build-flash-tools.sh"
[ "$(stat -c %s "$FILE")" = $((32768*512)) ] || die "expected a 16 MiB loader-only image"
cmp -s -n $((64*512)) "$FILE" /dev/zero || die "loader-only image must not contain a partition table"
[ "$(od -An -tx1 -j $((16384*512)) -N4 "$FILE" | tr -d ' \n')" = d00dfeed ] || die "U-Boot FIT not found at LBA 16384"
check_usb
devs=$($SUDO "$RKDT" ld)
[ "$(printf '%s\n' "$devs" | grep -ci 'DevNo=')" = 1 ] || die "connect exactly one Rockchip board"
load_loader
sectors=$($SUDO "$RKDT" rfi | awk '/Sectors/ {print $3}')
[[ "$sectors" =~ ^[0-9]+$ ]] && (( sectors > 32768+33 )) || die "invalid eMMC capacity"
[ "$(stat -c %s "$BACKUP")" = "$((sectors*512))" ] || die "backup does not match eMMC capacity"
[ -f "$BACKUP.sha256" ] || die "backup checksum missing; use scripts/backup-emmc.sh"
expected=$(awk 'NR==1 {print $1}' "$BACKUP.sha256")
[[ "$expected" =~ ^[[:xdigit:]]{64}$ ]] && [ "$(sha256sum "$BACKUP" | cut -d' ' -f1)" = "$expected" ] || die "backup checksum mismatch"

echo "Install $FILE and remove all eMMC partition tables."
echo "No operating system will boot from eMMC; a bootable SD card is required."
read -r -p "Type LOADER-ONLY to continue: " ans
[ "$ans" = LOADER-ONLY ] || die "aborted"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
dd if="$FILE" of="$scratch/primary" bs=512 count=64 status=none
dd if="$FILE" of="$scratch/loader" bs=512 skip=64 status=none
truncate -s $((33*512)) "$scratch/secondary"
write_and_verify "$scratch/loader" 64 0 $((16*2048)) 0
write_and_verify "$scratch/secondary" "$((sectors-33))" 0 33 0
write_and_verify "$scratch/primary" 0 0 64 0
echo "Loader and empty partition-table areas verified. Power off, insert the SD card and boot."
