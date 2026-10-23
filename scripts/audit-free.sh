#!/usr/bin/env bash
# Check that an image is fully free software (no closed binaries).
#   scripts/audit-free.sh out/tinker-edge-r-debian13-free.img
# Looks at: the bootloader (must be the one built from source), the root
# filesystem (known closed files, firmware directory, Debian package sections,
# apt components). Exit code 0 only if everything passes.
set -uo pipefail
cd "$(dirname "$0")/.."
IMG=${1:?usage: $0 <image>}
ROOT_START=${ROOT_START:-32768}          # mainline U-Boot layout (sectors)
R="$IMG?offset=$((ROOT_START * 512))"
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }
dbg() { debugfs -R "$1" "$R" 2>/dev/null; }

echo "== bootloader"
ub=out/u-boot-rockchip.bin
if [ -f "$ub" ] && cmp -s -n "$(stat -c %s "$ub")" "$ub" <(dd if="$IMG" bs=512 skip=64 status=none); then
    ok "sector 64 holds $ub (U-Boot with open TPL/SPL, built from source)"
else
    bad "sector 64 is not out/u-boot-rockchip.bin (stock or unknown bootloader?)"
fi

echo "== closed files"
for f in /usr/bin/upgrade_tool /usr/bin/npu_transfer_proxy /usr/lib/aarch64-linux-gnu/librknn_api.so \
         /usr/share/npu_fw /usr/share/npu_fw_pcie /lib/firmware/rtw88 /lib/firmware/rtl_bt /lib/firmware/brcm; do
    if dbg "stat $f" | grep -q Inode; then bad "$f present"; else ok "$f absent"; fi
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
