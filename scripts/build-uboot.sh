#!/usr/bin/env bash
# Build mainline U-Boot for the RK3399Pro (Rock Pi N10 defconfig) with the
# Rockchip BL31 from rkbin. FALLBACK ONLY: DDR parameters (LPDDR3 4 GB 1600) and
# the board DT of the N10 are assumed to match the Tinker Edge R; untested.
# Output: out/u-boot-rockchip.bin  (write at sector 64)
# Run inside the builder container:  ./run.sh scripts/build-uboot.sh
set -euo pipefail
cd "$(dirname "$0")/.."
U=downloads/u-boot
BL31=$PWD/downloads/rkbin/bin/rk33/rk3399pro_bl31_v1.35.elf
[ -f "$BL31" ] || { echo "missing $BL31 (run scripts/fetch-sources.sh)" >&2; exit 1; }

B=$PWD/build/u-boot-obj
mkdir -p "$B" out
export CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm BL31="$BL31"
make -C "$U" O="$B" rock-pi-n10-rk3399pro_defconfig
make -C "$U" O="$B" -j"$(nproc)"
cp "$B/u-boot-rockchip.bin" out/u-boot-rockchip.bin
chown "${HOST_UID:-0}:${HOST_GID:-0}" out/u-boot-rockchip.bin
ls -l out/u-boot-rockchip.bin
