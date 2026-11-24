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
# cat_root <path> prints the file content (possibly empty); 0 ok, 3 absent, 2 a read error
# (the file answers to stat but not to cat: an existing file that cannot be read is an
# incomplete check, not an empty one)
cat_root() {
    st=$(stat_one "$1")
    if [ "$st" = absent ]; then return 3; fi
    if [ "$st" = error ]; then return 2; fi
    content=$(dbg "cat $1")
    size=$(dbg "stat $1" | sed -n 's/^User:.*Size: \([0-9][0-9]*\).*/\1/p')
    if [ "${size:-0}" -gt 0 ] && [ -z "$content" ]; then return 2; fi
    printf '%s\n' "$content"
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
# forbidden components (contrib, non-free, non-free-firmware) in the active records of the apt
# sources: the one-line format of /etc/apt/sources.list and *.list, and the deb822 format of
# *.sources (stanza by stanza, with line continuations and the Enabled field). Comments never
# trigger; a file that answers to stat but not to cat is a read error (exit 2), not "free".
check_apt_file() {   # check_apt_file <rootfs-path> <line|deb822>: 0 clean, 1 forbidden component
    local c rc
    c=$(cat_root "$1"); rc=$?
    if [ "$rc" = 2 ]; then incomplete "cannot read $1 in the rootfs of $IMG (read error)"; fi
    [ "$rc" = 3 ] && return 0
    case $2 in
        line) printf '%s\n' "$c" | awk '
            { line = $0; sub(/\r$/, "", line); gsub(/^[ \t]+|[ \t]+$/, "", line)
              if (line == "" || line ~ /^#/) next
              if (line !~ /^(deb|deb-src)[ \t]/) next
              sub(/\[[^\]]*\]/, " ", line)   # the options group (it may contain spaces)
              n = split(line, t, /[ \t]+/)   # type uri suite components...
              for (i = 4; i <= n; i++) if (t[i] == "contrib" || t[i] == "non-free" || t[i] == "non-free-firmware") { bad = 1; break } }
            END { if (bad) exit 1 }'
            ;;
        deb822) printf '%s\n' "$c" | awk '
            function flush(   n, t, i) {
                if (enabled) {
                    n = split(comp, t, /[ \t]+/)
                    for (i = 1; i <= n; i++) if (t[i] == "contrib" || t[i] == "non-free" || t[i] == "non-free-firmware") { bad = 1; break }
                }
                enabled = 1; comp = ""; cur = "" }
            BEGIN { enabled = 1 }
            { line = $0; sub(/\r$/, "", line)
              if (line ~ /^[ \t]*#/) next                    # a comment line
              if (line ~ /^[ \t]*$/) { flush(); next }      # a blank line: the stanza is over
              if (line ~ /^[ \t]/) { if (cur == "Components") comp = comp " " line; next }   # a continued value
              col = index(line, ":")
              cur = substr(line, 1, col - 1); val = substr(line, col + 1)
              gsub(/^[ \t]+|[ \t]+$/, "", cur); gsub(/^[ \t]+/, "", val)
              if (cur == "Enabled") enabled = (tolower(val) == "no") ? 0 : 1
              else if (cur == "Components") comp = val   # the field order in a stanza is free: the other fields leave comp as it is
            }
            END { flush(); if (bad) exit 1 }'
            ;;
    esac
}
src=$(cat_root /etc/apt/sources.list) || { [ $? = 2 ] && incomplete "cannot read /etc/apt/sources.list in the rootfs of $IMG (read error)"; }
echo "$src" | sed 's/^/       /'
check_apt_file /etc/apt/sources.list line || bad "non-free components configured in /etc/apt/sources.list"
case $(stat_one "/etc/apt/sources.list.d") in
    absent) ok "/etc/apt/sources.list.d absent" ;;
    error)  incomplete "cannot stat /etc/apt/sources.list.d in the rootfs of $IMG (read error)" ;;
    present)
        extra=$(dbg "ls /etc/apt/sources.list.d" | tr -s ' \n' '\n' | grep -v -E '^[0-9]+$|^\(|^\.$|^\.\.$|^$' || true)
        if [ -z "$extra" ]; then
            ok "no extra apt sources"
        else
            for f in $extra; do
                case $f in
                    *.list)    check_apt_file "/etc/apt/sources.list.d/$f" line ;;
                    *.sources) check_apt_file "/etc/apt/sources.list.d/$f" deb822 ;;
                    *) continue ;;
                esac
                if [ $? = 0 ]; then ok "no non-free components in $f"; else bad "non-free components configured in $f"; fi
            done
        fi
        ;;
esac

echo
[ "$fail" = 0 ] && echo "RESULT: free" || echo "RESULT: NOT free"
exit $fail
