#!/usr/bin/env bash
# Build the open BL31 (ARM Trusted Firmware-A, BSD-3) for the RK3399.
# Output: out/bl31-tfa.elf   (used by scripts/build-uboot.sh)
# Patched with patches/tf-a/*.patch: with binutils 2.44 the veneers of the PMU SRAM
# entry code made the 8 KiB PMUSRAM region overflow by 3928 bytes (see the patch).
# TFA_MAKE_ARGS adds make arguments.
# Run inside the builder container:  ./run.sh scripts/build-tfa.sh
set -euo pipefail
cd "$(dirname "$0")/.."
T=downloads/tf-a
if [ ! -d "$T/.git" ]; then
    . ./sources.lock
    git clone --depth 1 --branch "$TFA_TAG" "$TFA_REPO" "$T"
fi
for pt in "$PWD"/patches/tf-a/*.patch; do
    [ -e "$pt" ] || continue
    if git -C "$T" apply -R --check "$pt" >/dev/null 2>&1; then
        echo "already applied: ${pt##*/}"
    else
        git -C "$T" apply "$pt" && echo "applied: ${pt##*/}"
    fi
done
B=$PWD/build/tf-a
rm -rf "$B"; mkdir -p "$B" out

make -C "$T" -j"$(nproc)" PLAT=rk3399 BUILD_BASE="$B" \
    CROSS_COMPILE=aarch64-linux-gnu- M0_CROSS_COMPILE=arm-none-eabi- \
    DEBUG=0 ${TFA_MAKE_ARGS:-} bl31
cp "$B/rk3399/release/bl31/bl31.elf" out/bl31-tfa.elf
chown "${HOST_UID:-0}:${HOST_GID:-0}" out/bl31-tfa.elf
ls -l out/bl31-tfa.elf
