#!/usr/bin/env bash
# Build the fully free image end to end (open TF-A + U-Boot, Debian "main" only).
#   scripts/make-free-image.sh        -> out/tinker-edge-r-debian13-free.img
# Needs Docker and the kernel already built (./run.sh scripts/build-kernel.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
# the open boot chain: open TF-A as BL31 and the open U-Boot (not the DDR-blob variant).
# Container-relative paths: run.sh mounts the repository at /work. The free build never takes a
# closed EL2, so an external BL31 is ignored: TF-A builds the open ELF from source (BL31 must be
# unset - a non-empty BL31 makes the TF-A Makefile skip the build and take it as a prebuilt image),
# and the U-Boot step is pinned to the ELF TF-A just built (scoped below, not exported).
export USE_TFA=1
export UBOOT_BIN=out/u-boot-rockchip.bin
./scripts/fetch-sources.sh
./scripts/setup-binfmt.sh
env -u BL31 ./run.sh scripts/build-tfa.sh
BL31=out/bl31-tfa.elf ./run.sh scripts/build-uboot.sh
# the open artifacts must exist before the image is packed (a BL31 the free build would
# have refused is caught by build-uboot.sh, the outputs are checked here)
[ -f out/bl31-tfa.elf ] || { echo "free build: out/bl31-tfa.elf missing: TF-A did not build" >&2; exit 1; }
[ -f out/u-boot-rockchip.bin ] || { echo "free build: out/u-boot-rockchip.bin missing: U-Boot did not build" >&2; exit 1; }
FREE=1 ./run.sh scripts/build-rootfs.sh
FREE=1 ./run.sh scripts/build-image-mainline-uboot.sh
./scripts/audit-free.sh out/tinker-edge-r-debian13-free.img
