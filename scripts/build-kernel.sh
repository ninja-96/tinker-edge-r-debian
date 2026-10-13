#!/usr/bin/env bash
# Build Linux (arm64) with our DTS. Outputs in out/: Image, *.dtb, modules/
# Run inside the builder container:  ./run.sh scripts/build-kernel.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./sources.lock
K=$PWD/build/linux-${LINUX_VERSION}
[ -d "$K" ] || { mkdir -p build; tar -xJf downloads/linux-${LINUX_VERSION}.tar.xz -C build; }
B=$PWD/build/linux-obj
mkdir -p "$B" out

# Patches and new files (IOMMU helpers, mpp_service port)
scripts/apply-kernel-mods.sh "$K"

# Our board DTS files
DT=$K/arch/arm64/boot/dts/rockchip
cp dts/*.dts dts/*.dtsi "$DT/"
for f in dts/*.dts; do
    b=$(basename "${f%.dts}")
    grep -q "$b.dtb" "$DT/Makefile" ||
        echo "dtb-\$(CONFIG_ARCH_ROCKCHIP) += $b.dtb" >> "$DT/Makefile"
done

export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
MAKE="make -C $K O=$B -j$(nproc)"
$MAKE defconfig
"$K/scripts/kconfig/merge_config.sh" -m -O "$B" "$B/.config" config/linux-tinker.fragment >/dev/null
$MAKE olddefconfig
$MAKE Image dtbs modules

rm -rf out/modules
$MAKE INSTALL_MOD_PATH=$PWD/out/modules INSTALL_MOD_STRIP=1 modules_install
cp "$B/arch/arm64/boot/Image" out/Image
cp "$B"/arch/arm64/boot/dts/rockchip/rk3399pro-tinker-edge-r*.dtb out/
cp "$B/.config" out/kernel.config
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" out build/linux-obj
ls -lh out/Image out/*.dtb
ls out/modules/lib/modules/*/kernel/drivers/soc/rockchip/mpp 2>/dev/null || true
