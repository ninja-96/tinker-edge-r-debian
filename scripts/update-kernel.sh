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

RS=; CTL=$(mktemp -t tinker-ssh.XXXXXX)
trap 'ssh -o ControlPath="$CTL" -O exit "$U@$HOST" >/dev/null 2>&1 || true; [ -z "$RS" ] || rm -f "$RS"' EXIT
echo "Connecting to $U@$HOST (enter the SSH password once)..."
ssh -o ControlMaster=yes -o ControlPath="$CTL" -o ControlPersist=yes -o StrictHostKeyChecking=accept-new -fN "$U@$HOST"
SSH=(ssh -o ControlPath="$CTL" "$U@$HOST")

echo "board runs: $("${SSH[@]}" uname -r)    new kernel: $REL"

RS=$(mktemp -t kupdate.XXXXXX)
cat > "$RS" <<'EOS'
set -eu
cd /tmp/kupd
CONF=/boot/extlinux/extlinux.conf
[ -f "$CONF" ] || { echo "error: no $CONF: not an extlinux image" >&2; exit 1; }

STAGE=/tmp/kupd/stage
MODDEST=/lib/modules/$REL
MODDEST_CREATED=0   # set when this attempt creates the module dir, so cleanup never removes a pre-existing one

# remove only what this attempt created: the staging and, if this attempt created the module
# dir (it does not exist when a collision is refused), that dir. A pre-existing /lib/modules/$REL
# is an installed release and must survive a failed attempt
cleanup() {
    rm -rf "$STAGE" /boot/Image.new /boot/dtbs.new
    if [ "$MODDEST_CREATED" = 1 ]; then rm -rf "$MODDEST"; fi
}
fail() { cleanup; echo "error: $*" >&2; exit 1; }

# --- preflight: everything is checked before the first write to /boot or /lib/modules ---
[ -f Image ] && ls rk3399pro-tinker-edge-r*.dtb >/dev/null && [ -d "modules/lib/modules/$REL" ] \
    || fail "incomplete upload (Image, *.dtb or modules/lib/modules/$REL)"
[ ! -e "$MODDEST" ] || fail "/lib/modules/$REL is already installed on the board: the new kernel was built with the same release as an installed one. Bump KERNEL_LOCALVERSION in sources.lock so every kernel has a unique release, rebuild (./run.sh scripts/build-kernel.sh) and retry."

# the 'previous' entry is regenerated from the 'label tinker' block; it must be complete and
# point at a device tree that is still on the board (checked before any file is replaced)
fdt=$(awk '/^label /{lbl=$2} lbl=="tinker" && /^ *fdt /{sub(/^ *fdt /,""); print; exit}' "$CONF")
app=$(awk '/^label /{lbl=$2} lbl=="tinker" && /^ *append /{sub(/^ *append /,""); print; exit}' "$CONF")
[ -n "$fdt" ] && [ -n "$app" ] || fail "no fdt/append in the 'label tinker' block of $CONF: the file is not an extlinux config of this image"
base=${fdt##*/}
[ -f "/boot/dtbs/$base" ] || fail "the current device tree /boot/dtbs/$base (the 'tinker' entry of $CONF) is missing: the board state is unexpected"

# --- stage the new release and verify it: a broken set never reaches the board ---
rm -rf "$STAGE"
install -d "$STAGE/lib/modules/$REL" || fail "cannot create $STAGE"
cp -a "modules/lib/modules/$REL/." "$STAGE/lib/modules/$REL/" || fail "cannot stage the modules of $REL"
depmod -b "$STAGE" "$REL" || fail "depmod of the staged $REL modules failed (incomplete or broken modules): the board is untouched"

# the new kernel and device trees, staged in /boot before anything is switched
install -m 0644 Image /boot/Image.new || fail "cannot write /boot/Image.new"
rm -rf /boot/dtbs.new
mkdir -p /boot/dtbs.new || fail "cannot create /boot/dtbs.new"
cp -a rk3399pro-tinker-edge-r*.dtb /boot/dtbs.new/ || fail "cannot stage the device trees"

# --- publish: the fallback set is snapshotted from the CURRENT (old) state before it is
# replaced, so a failure above leaves both old entries exactly as they were and the
# "previous" entry keeps the old kernel and the old device trees ---
cp -a /boot/Image /boot/Image.old || fail "cannot save the current kernel as /boot/Image.old"
rm -rf /boot/dtbs.old
cp -a /boot/dtbs /boot/dtbs.old || fail "cannot save the device trees as /boot/dtbs.old"
cp -a /boot/dtbs.new/. /boot/dtbs/ || fail "cannot install the new device trees into /boot/dtbs"
mv -f /boot/Image.new /boot/Image || fail "cannot switch /boot/Image to the new kernel"
MODDEST_CREATED=1
cp -a "$STAGE/lib/modules/$REL/." "$MODDEST/" || fail "cannot install the modules of $REL into /lib/modules"

# regenerate the fallback entry from the tinker block as left: after a rollback the board
# boots exactly the kernel, device tree and command line of the previous installation
tmp=$(mktemp "$(dirname "$CONF")/.extlinux.conf.XXXXXX") || fail "cannot create a temp file next to $CONF"
awk '
    /^label previous/ { skip = 1; next }
    skip && /^label / { skip = 0 }
    !skip { n++; buf[n] = $0 }
    END { while (n && buf[n] ~ /^[[:space:]]*$/) n--; for (i = 1; i <= n; i++) print buf[i] }
' "$CONF" > "$tmp"
printf '\nlabel previous\n    kernel /boot/Image.old\n    fdt /boot/dtbs.old/%s\n    append %s\n' "$base" "$app" >> "$tmp"
if [ "$(grep -c '^label previous' "$tmp")" != 1 ] || [ "$(grep -c '^label tinker' "$tmp")" != 1 ]; then
    rm -f "$tmp"
    fail "refusing to write $CONF: the result must have exactly one 'previous' and one 'tinker' entry"
fi
cp -f -- "$CONF" "$CONF.prev" || fail "cannot back up $CONF"
chmod --reference="$CONF" "$tmp"; chown --reference="$CONF" "$tmp"
mv -f -- "$tmp" "$CONF" || fail "cannot replace $CONF"
sync
rm -rf "$STAGE" /boot/Image.new /boot/dtbs.new
echo "installed $REL; reboot to use it"
EOS

echo "Uploading..."
( cd out && tar -cf - Image rk3399pro-tinker-edge-r*.dtb "modules/lib/modules/$REL" ) \
    | "${SSH[@]}" "rm -rf /tmp/kupd && mkdir -p /tmp/kupd && tar -xf - -C /tmp/kupd"
scp -q -o ControlPath="$CTL" "$RS" "$U@$HOST:/tmp/kupd/install.sh"
printf '%s\n' "$PASS" | "${SSH[@]}" "sudo -S -p '' env REL=$REL sh /tmp/kupd/install.sh && rm -rf /tmp/kupd"
echo "Done. Reboot the board:  ssh $U@$HOST sudo reboot"
