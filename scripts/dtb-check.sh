#!/usr/bin/env bash
# Compile every board DTS in dts/ against the pinned kernel tree.
# Run inside the builder container:  ./run.sh scripts/dtb-check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
K=build/linux-${LINUX_VERSION}
[ -d "$K" ] || { mkdir -p build; tar -xJf downloads/linux-${LINUX_VERSION}.tar.xz -C build; }
mkdir -p out
for DTS in dts/*.dts; do
    OUT=out/$(basename "${DTS%.dts}").dtb
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I dts -I "$K/include" -I "$K/arch/arm64/boot/dts" \
        -I "$K/arch/arm64/boot/dts/rockchip" -I "$K/scripts/dtc/include-prefixes" "$DTS" |
    dtc -@ -I dts -O dtb -W no-unit_address_vs_reg -W no-unique_unit_address -o "$OUT" -
    chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT" 2>/dev/null || true
    ls -l "$OUT"
done
