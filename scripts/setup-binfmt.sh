#!/usr/bin/env bash
# One-time host setup: register qemu-aarch64 in the host kernel (binfmt_misc)
# so arm64 binaries can run inside the build container (debootstrap second stage).
# Needs the docker group; changes host kernel state until reboot.
set -euo pipefail
if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    echo "qemu-aarch64 already registered"; exit 0
fi
docker run --privileged --rm tonistiigi/binfmt --install arm64
