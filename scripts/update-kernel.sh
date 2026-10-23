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

RS=; CTL=$(mktemp -u -t tinker-ssh.XXXXXX)
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

# keep the current kernel and device trees as the fallback
cp -a /boot/Image /boot/Image.old
rm -rf /boot/dtbs.old; cp -a /boot/dtbs /boot/dtbs.old

install -m 0644 Image /boot/Image.new
cp -f rk3399pro-tinker-edge-r*.dtb /boot/dtbs/
rm -rf "/lib/modules/$REL"; cp -a "modules/lib/modules/$REL" /lib/modules/
depmod "$REL"
mv -f /boot/Image.new /boot/Image

# add the fallback entry once
if ! grep -q '^label previous' "$CONF"; then
    fdt=$(sed -n 's#^ *fdt */boot/dtbs/##p' "$CONF" | head -1)
    app=$(sed -n 's#^ *append ##p' "$CONF" | head -1)
    cp -f "$CONF" "$CONF.prev"
    printf '\nlabel previous\n    kernel /boot/Image.old\n    fdt /boot/dtbs.old/%s\n    append %s\n' "$fdt" "$app" >> "$CONF"
fi
sync
echo "installed $REL; reboot to use it"
EOS

echo "Uploading..."
( cd out && tar -cf - Image rk3399pro-tinker-edge-r*.dtb "modules/lib/modules/$REL" ) \
    | "${SSH[@]}" "rm -rf /tmp/kupd && mkdir -p /tmp/kupd && tar -xf - -C /tmp/kupd"
scp -q -o ControlPath="$CTL" "$RS" "$U@$HOST:/tmp/kupd/install.sh"
"${SSH[@]}" "echo '$PASS' | sudo -S -p '' env REL=$REL sh /tmp/kupd/install.sh && rm -rf /tmp/kupd"
echo "Done. Reboot the board:  ssh $U@$HOST sudo reboot"
