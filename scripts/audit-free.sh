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
if [ -s "$ub" ] && cmp -s -n "$(stat -c %s "$ub")" "$ub" "$IMG" 0 $((64 * 512)); then
    ok "sector 64 holds $ub (U-Boot with open TPL/SPL, built from source)"
else
    bad "sector 64 is not out/u-boot-rockchip.bin (stock or unknown bootloader?)"
fi

# the rootfs must answer at the builder's offset before any check: an empty
# debugfs output would otherwise read as "everything absent" and a false "free"
scratch=$(mktemp -d) || incomplete "cannot create audit directory"
trap 'rm -rf "$scratch"' EXIT
# debugfs also reports filesystem errors on stderr with a successful exit status.
dbg() {
    LC_ALL=C debugfs -R "$1" "$R" 2>"$scratch/errors" || return 1
    ! grep -qv '^debugfs [0-9]' "$scratch/errors"
}
read_text() {
    local size
    size=$(dbg "stat $1" | sed -n 's/^User:.*Size: \([0-9][0-9]*\).*/\1/p') || return 1
    dbg "cat $1" > "$scratch/text" || return 1
    [ "$(wc -c < "$scratch/text")" = "$size" ] || return 1
    # Package metadata and source lists cannot contain NUL (including partial zero blocks).
    tr -d '\000' < "$scratch/text" | cmp -s "$scratch/text" -
}
try_root() {
    R=$1
    read_text /var/lib/dpkg/status && grep -q '^Package:' "$scratch/text" &&
        cp "$scratch/text" "$scratch/status" &&
        read_text /etc/apt/sources.list && grep -q . "$scratch/text"
}
try_root "$IMG?offset=$((ROOT_START * 512))" || {
    echo "  note extracting the root partition for debugfs"
    dd if="$IMG" bs=4M iflag=skip_bytes skip="$((ROOT_START * 512))" status=none of="$scratch/rootfs" || incomplete "cannot extract root partition"
    try_root "$scratch/rootfs" || incomplete "cannot fully read mandatory package metadata at sector $ROOT_START"
}
stat_one() {
    local out
    if out=$(dbg "stat $1") && [[ "$out" = Inode:* ]]; then echo present
    elif grep -qi 'file not found' "$scratch/errors"; then echo absent
    else echo error; fi
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
n=$(grep -c -E '^Section: (non-free|contrib)' "$scratch/status")
[ "$n" = 0 ] && ok "no non-free/contrib package sections" || bad "$n packages with non-free/contrib sections"

echo "== apt sources"
# forbidden components (contrib, non-free, non-free-firmware) in the active records of the apt
# sources: the one-line format of /etc/apt/sources.list and *.list, and the deb822 format of
# *.sources (stanza by stanza, with line continuations and the Enabled field). Comments never
# trigger; a file that answers to stat but not to cat is a read error (exit 2), not "free".
check_apt_file() {   # check_apt_file <rootfs-path> <line|deb822>: 0 clean, 1 forbidden component
    read_text "$1" || incomplete "cannot fully read $1 in the rootfs"
    case $2 in
        line) awk '
            { line = $0; sub(/\r$/, "", line); gsub(/^[ \t]+|[ \t]+$/, "", line)
              if (line == "" || line ~ /^#/) next
              sub(/#.*/, "", line); gsub(/[ \t]+$/, "", line)   # a trailing comment is not a component
              if (line !~ /^(deb|deb-src)[ \t]/) next
              sub(/\[[^\]]*\]/, " ", line)   # the options group (it may contain spaces)
              n = split(line, t, /[ \t]+/)   # type uri suite components...
              for (i = 4; i <= n; i++) if (t[i] == "contrib" || t[i] == "non-free" || t[i] == "non-free-firmware") { bad = 1; break } }
            END { if (bad) exit 1 }' "$scratch/text"
            ;;
        deb822) awk '
            function flush(   n, t, i) {
                if (enabled) {
                    n = split(comp, t, /[ \t]+/)
                    for (i = 1; i <= n; i++) if (t[i] == "contrib" || t[i] == "non-free" || t[i] == "non-free-firmware") { bad = 1; break }
                }
                enabled = 1; comp = ""; lcur = "" }
            BEGIN { enabled = 1 }
            { line = $0; sub(/\r$/, "", line)
              if (line ~ /^[ \t]*#/) next                    # a comment line
              if (line ~ /^[ \t]*$/) { flush(); next }      # a blank line: the stanza is over
              if (line ~ /^[ \t]/) { if (lcur == "components") comp = comp " " line; next }   # a continued value
              col = index(line, ":")
              cur = substr(line, 1, col - 1); val = substr(line, col + 1)
              gsub(/^[ \t]+|[ \t]+$/, "", cur); gsub(/^[ \t]+|[ \t]+$/, "", val)
              lcur = tolower(cur)   # APT reads field names case-insensitively (components: is Components:)
              if (lcur == "enabled") enabled = (tolower(val) ~ /^(no|false|off|0)$/) ? 0 : 1
              else if (lcur == "components") comp = val   # the field order in a stanza is free: the other fields leave comp as it is
            }
            END { flush(); if (bad) exit 1 }' "$scratch/text"
            ;;
    esac
}
check_apt_file /etc/apt/sources.list line || bad "non-free components configured in /etc/apt/sources.list"
case $(stat_one "/etc/apt/sources.list.d") in
    absent) ok "/etc/apt/sources.list.d absent" ;;
    error)  incomplete "cannot stat /etc/apt/sources.list.d in the rootfs of $IMG (read error)" ;;
    present)
        extra=$(dbg "ls -p /etc/apt/sources.list.d" | awk -F/ '
            $6 == "." { dot = 1 }
            $6 == ".." { parent = 1 }
            $6 ~ /^[A-Za-z0-9_.-]+\.(list|sources)$/ { print $6 }
            END { if (!dot || !parent) exit 1 }') || incomplete "cannot list sources.list.d"
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
