#!/usr/bin/env bash
# Check that an image is fully free software (no closed binaries).
#   scripts/audit-free.sh out/tinker-edge-r-debian13-free.img
# Looks at: the bootloader (must be the one built from source), the root
# filesystem (known closed files, firmware directory, Debian package sections,
# apt components). Exit 0 only if everything passes; exit 2 when the image cannot
# be checked at all (the reason is printed) — an unchecked image is not "free".
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=${1:?usage: $0 <image>}
ROOT_START=${ROOT_START:-32768}          # mainline U-Boot layout (sectors)
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }
incomplete() { echo "error: $*" >&2; exit 2; }
command -v debugfs >/dev/null || incomplete "debugfs (e2fsprogs) is missing: install it and re-run"

echo "== bootloader"
ub=out/u-boot-rockchip.bin
if [ -f "$ub" ] && cmp -s -n "$(stat -c %s "$ub")" "$ub" <(dd if="$IMG" bs=512 skip=64 status=none); then
    ok "sector 64 holds $ub (U-Boot with open TPL/SPL, built from source)"
else
    bad "sector 64 is not out/u-boot-rockchip.bin (stock or unknown bootloader?)"
fi

# the rootfs must answer at the builder's offset before any check: an empty
# debugfs output would otherwise read as "everything absent" and a false "free"
R=""
try_root() {   # try_root <source>: 0 when the rootfs is readable and the required files exist
    debugfs -R "cat /var/lib/dpkg/status" "$1" 2>/dev/null | grep -q '^Package:' || return 1
    debugfs -R "cat /etc/apt/sources.list" "$1" 2>/dev/null | grep -q . || return 1
    R=$1
}
tmp=""
try_root "$IMG?offset=$((ROOT_START * 512))" || {
    # this debugfs cannot read the partition with the offset: extract it into a
    # temporary file and read that (never a neighbouring out/rootfs.ext4)
    tmp=$(mktemp "${TMPDIR:-/tmp}/audit-rootfs.XXXXXX")
    trap '[ -n "$tmp" ] && rm -f "$tmp"' EXIT
    echo "  note debugfs cannot read the partition with the offset: extracting it to $tmp"
    dd if="$IMG" bs=512 skip="$ROOT_START" status=none of="$tmp" || incomplete "cannot read the root partition of $IMG at sector $ROOT_START"
    try_root "$tmp" || incomplete "no readable ext4 rootfs at sector $ROOT_START of $IMG: /var/lib/dpkg/status and /etc/apt/sources.list must answer there"
}
dbg() { debugfs -R "$1" "$R" 2>/dev/null; }
# stat_one <path> prints "present" | "absent" | "error" (error: the rootfs did not answer)
stat_one() {
    out=$(debugfs -R "stat $1" "$R" 2>&1)   # the "file not found" message of a missing path goes to stderr
    if printf '%s\n' "$out" | grep -q '^Inode:'; then
        echo present
    elif printf '%s\n' "$out" | grep -qi 'no such file\|file not found'; then
        echo absent
    else
        echo error
    fi
}

echo "== closed files"
for f in /usr/bin/upgrade_tool /usr/bin/npu_transfer_proxy /usr/lib/aarch64-linux-gnu/librknn_api.so \
         /usr/share/npu_fw /usr/share/npu_fw_pcie /lib/firmware/rtw88 /lib/firmware/rtl_bt /lib/firmware/brcm; do
    case $(stat_one "$f") in
        absent)  ok "$f absent" ;;
        present) bad "$f present" ;;
        *)       incomplete "cannot stat $f in the rootfs of $IMG (read error)" ;;
    esac
done

echo "== /lib/firmware"
dbg "ls /lib/firmware" | tr -s ' \n' '\n' | grep -v -E '^[0-9]+$|^\(|^\.$|^\.\.$|^$' | head -20 | sed 's/^/       /'

echo "== Debian packages from non-free/contrib"
n=$(dbg "cat /var/lib/dpkg/status" | grep -c -E '^Section: (non-free|contrib)')
[ "$n" = 0 ] && ok "no non-free/contrib package sections" || bad "$n packages with non-free/contrib sections"

echo "== apt sources"
src=$(dbg "cat /etc/apt/sources.list")
echo "$src" | sed 's/^/       /'
echo "$src" | grep -q -E 'contrib|non-free' && bad "non-free components configured" || ok "components: main only"

echo
[ "$fail" = 0 ] && echo "RESULT: free" || echo "RESULT: NOT free"
exit $fail
