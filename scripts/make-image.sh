#!/usr/bin/env bash
# Build the image that boots on the Tinker Edge R, from a clean checkout:
#   scripts/make-image.sh        -> out/tinker-edge-r-debian13.img
# Needs Docker (user in the docker group). Takes about 40 minutes.
set -euo pipefail
cd "$(dirname "$0")/.."

./scripts/fetch-sources.sh
./scripts/setup-binfmt.sh
./run.sh scripts/build-rootfs.sh                  # Debian 13   -> out/rootfs.tar
./run.sh scripts/build-kernel.sh                  # Linux 6.18  -> out/Image, out/*.dtb, out/modules
./run.sh scripts/build-mpp.sh                      # Rockchip mpp -> out/mpp
./run.sh scripts/build-gst-rockchip.sh             # GStreamer plugins -> out/gst-rockchip
./run.sh scripts/build-uboot.sh                   # U-Boot      -> out/u-boot-rockchip-ddrblob.bin
./scripts/fetch-npu.sh                              # Rockchip NPU files (pinned, sha256 checked)
./run.sh scripts/build-rkdeveloptool-arm64.sh      # rkdeveloptool for the board -> out/rkdeveloptool-arm64
./run.sh scripts/build-flash-tools.sh             # rkdeveloptool + loaders -> out/tools
./run.sh scripts/build-image-mainline-uboot.sh    # image       -> out/tinker-edge-r-debian13.img
ls -lh out/tinker-edge-r-debian13.img
