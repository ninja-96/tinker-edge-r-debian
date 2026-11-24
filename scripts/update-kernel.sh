#!/usr/bin/env bash
# Install a freshly built kernel (out/Image, out/*.dtb, out/modules) on a RUNNING board
# over SSH, without reflashing. The previous kernel stays as the extlinux entry
# "previous" (select it on the serial console / keyboard at the U-Boot menu if the new
# one does not boot). Details: docs/KERNEL.md
#   scripts/update-kernel.sh [HOST]        HOST defaults to tinker-edge-r.local
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
[ -f "$CONF" ] || { echo "no $CONF: not an extlinux image" >&2; exit 1; }

# before writing to /boot or /lib/modules: the upload is complete and the release is new
[ -f Image ] && ls rk3399pro-tinker-edge-r*.dtb >/dev/null && [ -d "modules/lib/modules/$REL" ] \
    || { echo "incomplete upload (Image, *.dtb or modules/lib/modules/$REL)" >&2; exit 1; }
if [ -e "/lib/modules/$REL" ]; then
    echo "error: /lib/modules/$REL is already installed on the board: the new kernel was built with the same release as an installed one." >&2
    echo "bump KERNEL_LOCALVERSION in sources.lock so every kernel has a unique release, rebuild (./run.sh scripts/build-kernel.sh) and retry." >&2
    exit 1
fi

# keep the current kernel and device trees as the fallback
cp -a /boot/Image /boot/Image.old
rm -rf /boot/dtbs.old; cp -a /boot/dtbs /boot/dtbs.old

# the modules first: if this fails, the boot files are still untouched
install -d "/lib/modules/$REL"
cp -a "modules/lib/modules/$REL/." "/lib/modules/$REL/"
depmod "$REL"

# the new kernel and device trees, staged before the switch
install -m 0644 Image /boot/Image.new
rm -rf /boot/dtbs.new; mkdir -p /boot/dtbs.new
cp -a rk3399pro-tinker-edge-r*.dtb /boot/dtbs.new/
mv -f /boot/Image.new /boot/Image
cp -a /boot/dtbs.new/. /boot/dtbs/
rm -rf /boot/dtbs.new

# regenerate the fallback entry from the tinker block as left: after a rollback the board
# boots exactly the kernel, device tree and command line of the previous installation
fdt=$(awk '/^label /{lbl=$2} lbl=="tinker" && /^ *fdt /{sub(/^ *fdt /,""); print; exit}' "$CONF")
app=$(awk '/^label /{lbl=$2} lbl=="tinker" && /^ *append /{sub(/^ *append /,""); print; exit}' "$CONF")
if [ -z "$fdt" ] || [ -z "$app" ]; then
    echo "no fdt/append in the 'label tinker' block of $CONF: the file is not an extlinux config of this image" >&2
    exit 1
fi
base=${fdt##*/}
tmp=$(mktemp "$(dirname "$CONF")/.extlinux.conf.XXXXXX")
awk '
    /^label previous/ { skip = 1; next }
    skip && /^label / { skip = 0 }
    !skip { n++; buf[n] = $0 }
    END { while (n && buf[n] ~ /^[[:space:]]*$/) n--; for (i = 1; i <= n; i++) print buf[i] }
' "$CONF" > "$tmp"
printf '\nlabel previous\n    kernel /boot/Image.old\n    fdt /boot/dtbs.old/%s\n    append %s\n' "$base" "$app" >> "$tmp"
if [ "$(grep -c '^label previous' "$tmp")" != 1 ] || [ "$(grep -c '^label tinker' "$tmp")" != 1 ]; then
    rm -f "$tmp"; echo "refusing to write $CONF: the result must have exactly one 'previous' and one 'tinker' entry" >&2; exit 1
fi
cp -f -- "$CONF" "$CONF.prev"
chmod --reference="$CONF" "$tmp"; chown --reference="$CONF" "$tmp"
mv -f -- "$tmp" "$CONF"
sync
echo "installed $REL; reboot to use it"
EOS

echo "Uploading..."
( cd out && tar -cf - Image rk3399pro-tinker-edge-r*.dtb "modules/lib/modules/$REL" ) \
    | "${SSH[@]}" "rm -rf /tmp/kupd && mkdir -p /tmp/kupd && tar -xf - -C /tmp/kupd"
scp -q -o ControlPath="$CTL" "$RS" "$U@$HOST:/tmp/kupd/install.sh"
printf '%s\n' "$PASS" | "${SSH[@]}" "sudo -S -p '' env REL=$REL sh /tmp/kupd/install.sh && rm -rf /tmp/kupd"
echo "Done. Reboot the board:  ssh $U@$HOST sudo reboot"
