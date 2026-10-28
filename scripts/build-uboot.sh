#!/usr/bin/env bash
# Build mainline U-Boot for the Tinker Edge R (RK3399Pro). The DDR
# initialisation (TPL) is U-Boot's own open code; BL31 defaults to the open
# TF-A build (out/bl31-tfa.elf, scripts/build-tfa.sh). Set BL31=<file> to use
# another one (e.g. the Rockchip blob from rkbin). The DDR parameters (LPDDR3
# 4 GB 1600) and the board DT of the N10 are assumed to match the Tinker Edge R;
# untested.
# Output: out/u-boot-rockchip.bin  (write at sector 64)
# Run inside the builder container:  ./run.sh scripts/build-uboot.sh
set -euo pipefail
cd "$(dirname "$0")/.."
U=downloads/u-boot
# BL31: by default the Rockchip blob (proven on the board). USE_TFA=1 uses the open
# TF-A build from scripts/build-tfa.sh (out/bl31-tfa.elf); BL31=<file> overrides both.
if [ -z "${BL31:-}" ]; then
    if [ "${USE_TFA:-0}" = 1 ]; then BL31=$PWD/out/bl31-tfa.elf
    else BL31=$PWD/blobs/rockchip/rk3399pro_bl31_v1.35.elf; fi
fi
[ -f "$BL31" ] || { echo "missing $BL31 (committed in blobs/, or scripts/build-tfa.sh for USE_TFA=1)" >&2; exit 1; }
echo "BL31: $BL31"

B=$PWD/build/u-boot-obj
mkdir -p "$B" out
export CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm BL31="$BL31"
# Our board: defconfig and U-Boot dtsi from u-boot-mods/, the kernel device tree
# (the same file the kernel uses) as the U-Boot control device tree.
cp -a u-boot-mods/. "$U/"
# The boot logo: the white Debian swirl, centred (patches/u-boot, artwork/boot-logo)
scripts/make-boot-logo.sh >/dev/null
cp out/boot-logo/u_boot_logo.bmp "$U/drivers/video/u_boot_logo.bmp"
for p in patches/u-boot/*.patch; do
    [ -e "$p" ] || continue
    if patch -d "$U" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then echo "already applied: $p"; else patch -d "$U" -p1 -s -f < "$p"; echo "applied: $p"; fi
done
cp dts/rk3399pro-tinker-edge-r.dts "$U/dts/upstream/src/arm64/rockchip/"
make -C "$U" O="$B" tinker-edge-r-rk3399pro_defconfig
make -C "$U" O="$B" -j"$(nproc)"
cp "$B/u-boot-rockchip.bin" out/u-boot-rockchip.bin

# Variant with the Rockchip DDR initialisation blob instead of U-Boot's open TPL
# (the blob detects the DRAM type and trains it; U-Boot's own LPDDR4 timings were
# seen to hang on the Tinker Edge R). Layout is the same: idbloader at sector 64
# (file offset 0), u-boot.itb at sector 16384.
# 933 MHz (LPDDR4-1866) by default; use rk3399pro_ddr_800MHz_v1.30.bin for the ASUS speed
DDR_BLOB=${DDR_BLOB:-$PWD/blobs/rockchip/rk3399pro_ddr_933MHz_v1.30.bin}
if [ -f "$DDR_BLOB" ]; then
    "$B/tools/mkimage" -n rk3399 -T rksd -d "$DDR_BLOB:$B/spl/u-boot-spl.bin" "$B/idbloader-ddrblob.img" >/dev/null
    python3 - "$B/idbloader-ddrblob.img" "$B/u-boot.itb" out/u-boot-rockchip-ddrblob.bin <<'PY'
import sys
idb, itb, out = sys.argv[1:4]
data = open(idb, "rb").read()
off = (16384 - 64) * 512
assert len(data) <= off, "idbloader too big"
open(out, "wb").write(data.ljust(off, b"\0") + open(itb, "rb").read())
PY
    echo "DDR-blob variant: out/u-boot-rockchip-ddrblob.bin"
fi
chown "${HOST_UID:-0}:${HOST_GID:-0}" out/u-boot-rockchip*.bin
ls -l out/u-boot-rockchip*.bin
