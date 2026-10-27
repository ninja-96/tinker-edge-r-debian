#!/usr/bin/env bash
# Boot out/Image + the Debian rootfs (with our overlay) in QEMU "virt" and run
# a few checks. This validates the userspace (systemd, networkd, ssh, modules,
# helper scripts), NOT the Tinker Edge R hardware or its device tree.
# Run inside the builder container:  ./run.sh scripts/qemu-smoke.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
O=out
KVER=${LINUX_VERSION}${KERNEL_LOCALVERSION}
W=build/rootfs-qemu
IMG=build/qemu-rootfs.ext4
LOG=build/qemu-smoke.log

mkdir -p build
scripts/stage-rootfs.sh "$W"

# one-shot test unit
cat > "$W/usr/local/sbin/smoke.sh" <<'T'
#!/bin/bash
p() { echo "SMOKE: $*"; }
p "debian=$(cat /etc/debian_version)"
p "kernel=$(uname -r)"
p "ssh=$(systemctl is-active ssh)"
p "networkd=$(systemctl is-active systemd-networkd)"
p "resolved=$(systemctl is-active systemd-resolved)"
p "hostname=$(hostname)"
p "user=$(id -un debian 2>&1)"
p "hostkeys=$(ls /etc/ssh/ssh_host_*_key 2>/dev/null | wc -l)"
p "ssh_listen=$(ss -ltn 2>/dev/null | grep -c ':22 ')"
p "ssh_banner=$(ssh-keyscan -T 5 127.0.0.1 2>/dev/null | wc -l) host key(s)"
p "ip4=$(ip -4 -br a | awk '$1 ~ /^e/ {print $1,$3}' | tr '\n' ' ')"
p "dns=$(getent hosts deb.debian.org | head -1 | awk '{print $1}')"
p "timesyncd=$(systemctl is-active systemd-timesyncd)"
p "regdb=$(ls /lib/firmware/regulatory.db 2>&1 | head -1)"
p "fw_rtw88=$(ls /lib/firmware/rtw88 2>/dev/null | head -3 | tr '\n' ' ')"
p "fw_rtl_bt=$(ls /lib/firmware/rtl_bt 2>/dev/null | grep -c 8822)"
p "python3=$(python3 --version 2>&1)"
p "sfdisk=$(command -v sfdisk || echo missing)"
p "modinfo_rk_vcodec=$(modinfo -F version rk_vcodec 2>&1 | head -1)"
p "modinfo_npu_power=$(modinfo -F description tinker_npu_power 2>&1 | head -1)"
p "modules_dep=$(test -s /lib/modules/$(uname -r)/modules.dep && echo ok || echo missing)"
p "npu_module=$(python3 -c 'import rk3399pro_npu as n; print(n.__version__)' 2>&1 | tail -1)"
p "npu_info=$(test -x /usr/local/bin/npu-info && echo present || echo missing)"
p "npu_fw=$(ls /usr/share/npu_fw 2>/dev/null | tr '\n' ' ')"
p "librknn=$(ls /usr/lib/aarch64-linux-gnu/librknn_api.so 2>&1)"
p "boot_select=$(tinker-boot-select 2>&1 | tr '\n' ' ')"
p "npu_unit_enabled=$(systemctl is-enabled npu-firmware 2>&1)"
p "npu_condition=$(systemctl show -p ConditionResult npu-firmware | cut -d= -f2)"
p "failed_units=$(systemctl --failed --no-legend | awk '{print $2}' | tr '\n' ' ')"
p "DONE"
systemctl poweroff
T
chmod +x "$W/usr/local/sbin/smoke.sh"
cat > "$W/etc/systemd/system/smoke.service" <<'T'
[Unit]
Description=smoke test
After=multi-user.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/smoke.sh
StandardOutput=journal+console
[Install]
WantedBy=multi-user.target
T
ln -sf /etc/systemd/system/smoke.service "$W/etc/systemd/system/multi-user.target.wants/smoke.service"

rm -f "$IMG"
mke2fs -q -t ext4 -L rootfs -d "$W" -E root_owner=0:0 "$IMG" 1536M

echo "booting QEMU (arm64 under TCG, this takes a few minutes)..."
set +e
timeout 900 qemu-system-aarch64 -M virt -cpu cortex-a72 -m 2048 -smp 4 -nographic \
    -kernel $O/Image \
    -append "console=ttyAMA0 root=/dev/vda rw rootfstype=ext4 rootwait systemd.show_status=0" \
    -drive file="$IMG",if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
    -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
    > "$LOG" 2>&1
rc=$?
set -e
echo "qemu exit code: $rc (log: $LOG)"
grep -a "SMOKE:" "$LOG" | sed 's/\r$//' || { echo "no SMOKE output; tail of log:"; tail -30 "$LOG"; exit 1; }
