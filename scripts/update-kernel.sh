#!/usr/bin/env bash
# Install a freshly built kernel (out/Image, out/*.dtb, out/modules) on a RUNNING board
# over SSH, without reflashing. The previous kernel stays as the extlinux entry
# "previous" (select it on the serial console / keyboard at the U-Boot menu if the new
# one does not boot). Details: docs/KERNEL.md
#   scripts/update-kernel.sh [HOST]        HOST defaults to tinker-edge-r.local
# The update is published only after the whole new set (modules, kernel, device trees) is
# staged and verified: a failed attempt leaves both boot entries as they were, and the same
# release can be retried. A release already installed on the board is still refused.
# SSH_USER (debian) and BOARD_PASS (debian, for sudo on the board) can be set.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${1:-tinker-edge-r.local}
U=${SSH_USER:-debian}
PASS=${BOARD_PASS:-debian}
die() { echo "error: $*" >&2; exit 1; }

[ -f out/Image ] || die "out/Image missing: build first (./run.sh scripts/build-kernel.sh)"
ls out/rk3399pro-tinker-edge-r*.dtb >/dev/null 2>&1 || die "no out/*.dtb"
[ "$(ls out/modules/lib/modules | wc -l)" = 1 ] || die "expected exactly one release in out/modules/lib/modules"
REL=$(ls out/modules/lib/modules)

LOCAL=$(mktemp -d)
CTL=$LOCAL/ssh
UPLOAD=
cleanup_upload() {
    if [ -n "$UPLOAD" ]; then "${SSH[@]}" "rm -rf -- '$UPLOAD'" || true; fi
    ssh -o ControlPath="$CTL" -O exit "$U@$HOST" >/dev/null 2>&1 || true
    rm -rf "$LOCAL"
}
trap cleanup_upload EXIT
echo "Connecting to $U@$HOST (enter the SSH password once)..."
ssh -o ControlMaster=yes -o ControlPath="$CTL" -o ControlPersist=yes -o StrictHostKeyChecking=accept-new -fN "$U@$HOST"
SSH=(ssh -o ControlPath="$CTL" "$U@$HOST")

echo "board runs: $("${SSH[@]}" uname -r)    new kernel: $REL"

UPLOAD=$("${SSH[@]}" 'mktemp -d /tmp/kupd.XXXXXXXX')
[[ "$UPLOAD" =~ ^/tmp/kupd\.[[:alnum:]]+$ ]] || { UPLOAD=; die "unexpected upload path"; }
[[ "$REL" =~ ^[[:alnum:]_+.~-]+$ && "$REL" != . && "$REL" != .. ]] || die "invalid kernel release"
RS=$LOCAL/install.sh
cat > "$RS" <<'EOS'
set -eu
cd "$(dirname "$0")"
CONF=/boot/extlinux/extlinux.conf
exec 9< /boot/extlinux
flock -x 9
die() { echo "error: $*" >&2; exit 1; }
[ -f "$CONF" ] || die "no $CONF"
MODDEST=/lib/modules/$REL
[ ! -e "$MODDEST" ] || die "$MODDEST is already installed; bump KERNEL_LOCALVERSION and rebuild"
[ -s Image ] && [ -s "modules/lib/modules/$REL/modules.order" ] || die "incomplete upload"
work=$(mktemp -d /boot/.kernel-update.XXXXXXXX)
module_dir_created=0
publishing=0
cleanup() {
    rc=$?
    trap - EXIT HUP INT TERM
    if [ "$publishing" = 1 ] && [ ! -e "$work/conf" ]; then module_dir_created=0; publishing=0; fi
    if [ "$publishing" = 1 ]; then
        if [ -e "$work/saved/conf.prev" ]; then
            mv -f "$work/saved/conf.prev" "$CONF.prev"
        elif [ ! -e "$work/conf.prev" ]; then
            rm -f "$CONF.prev"
        fi
        for target in Image dtbs Image.old dtbs.old; do
            if [ -e "$work/saved/$target" ]; then
                rm -rf "/boot/$target"
                mv "$work/saved/$target" "/boot/$target"
            elif [ ! -e "$work/next/$target" ]; then
                rm -rf "/boot/$target"
            fi
        done
    fi
    if [ "$module_dir_created" = 1 ]; then rm -rf "$MODDEST"; fi
    rm -rf "$work"
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir "$work/next" "$work/saved"

# Parse the active entry before publishing any files. Preserve unrelated labels verbatim.
awk '
    tolower($1)=="label" { label=$2; if (label=="tinker") active++; if (label=="previous") next }
    label=="previous" { next }
    label=="tinker" && tolower($1)=="fdt" { nf++; fdt=$2 }
    label=="tinker" && tolower($1)=="append" { na++; app=$0; sub(/^[[:space:]]*[^[:space:]]+[[:space:]]+/, "", app) }
    { print }
    END {
        if (active!=1 || nf!=1 || na!=1 || fdt !~ /^\/boot\/dtbs\/[[:alnum:]_.-]+\.dtb$/) exit 1
        sub(/^.*\//,"",fdt)
        print "\nlabel previous\n    kernel /boot/Image.old\n    fdt /boot/dtbs.old/" fdt "\n    append " app
    }
' "$CONF" > "$work/conf" || die "invalid active extlinux entry"
base=$(awk 'tolower($1)=="label"{label=$2} label=="previous" && tolower($1)=="fdt"{print $2}' "$work/conf")
base=${base##*/}
[ -s "/boot/dtbs/$base" ] && [ -s "$base" ] || die "active device tree $base missing from current or new release"
chmod --reference="$CONF" "$work/conf"; chown --reference="$CONF" "$work/conf"

mkdir "$MODDEST"
module_dir_created=1
cp -a "modules/lib/modules/$REL/." "$MODDEST/"
depmod "$REL"
install -m 0644 Image "$work/next/Image"
cp -a /boot/Image "$work/next/Image.old"
cp -a /boot/dtbs "$work/next/dtbs.old"
mkdir "$work/next/dtbs"
cp -a rk3399pro-tinker-edge-r*.dtb "$work/next/dtbs/"
cp -a "$CONF" "$work/conf.prev"
[ ! -e "$CONF.prev" ] || cp -a "$CONF.prev" "$work/saved/conf.prev"

publishing=1
mv -f "$work/conf.prev" "$CONF.prev"
for target in Image.old dtbs.old dtbs Image; do
    if [ -e "/boot/$target" ]; then mv "/boot/$target" "$work/saved/$target"; fi
    mv "$work/next/$target" "/boot/$target"
done
mv -f "$work/conf" "$CONF"
module_dir_created=0
publishing=0
sync
echo "installed $REL; reboot to use it"
EOS

echo "Uploading..."
( cd out && tar -cf - Image rk3399pro-tinker-edge-r*.dtb "modules/lib/modules/$REL" ) \
    | "${SSH[@]}" "tar -xf - -C '$UPLOAD'"
scp -q -o ControlPath="$CTL" "$RS" "$U@$HOST:$UPLOAD/install.sh"
printf '%s\n' "$PASS" | "${SSH[@]}" "sudo -S -p '' env REL=$REL sh '$UPLOAD/install.sh'"
echo "Done. Reboot the board:  ssh $U@$HOST sudo reboot"
