#!/usr/bin/env bash
# Build the fully free image end to end (open TF-A + U-Boot, Debian "main" only).
#   scripts/make-free-image.sh        -> out/tinker-edge-r-debian13-free.img
# Needs Docker and the kernel already built (./run.sh scripts/build-kernel.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/fetch-sources.sh
./scripts/setup-binfmt.sh
./run.sh scripts/build-tfa.sh
./run.sh scripts/build-uboot.sh
FREE=1 ./run.sh scripts/build-rootfs.sh
FREE=1 ./run.sh scripts/build-image-mainline-uboot.sh
./scripts/audit-free.sh out/tinker-edge-r-debian13-free.img
