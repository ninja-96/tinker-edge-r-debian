#!/usr/bin/env bash
# Build Rockchip's open-source media library "mpp" (Apache-2.0) for arm64: librockchip_mpp
# and the test tools (mpi_enc_test, mpi_dec_test, ...). They talk to /dev/mpp_service,
# the kernel driver of the -mpp device tree variant. No binary blobs involved.
# Output: out/mpp/  (install tree with prefix /usr, copied into the rootfs by stage-rootfs.sh)
# Run inside the builder container:  ./run.sh scripts/build-mpp.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
. ./sources.lock
D=downloads/mpp
g() { git -c safe.directory="*" -C "$D" "$@"; }
if [ ! -d "$D/.git" ]; then
    git init -q "$D"
    g remote add origin "$MPP_REPO"
fi
if [ "$(g rev-parse HEAD 2>/dev/null || true)" != "$MPP_REV" ]; then
    g fetch -q --depth 1 origin "$MPP_REV"
    g checkout -q --force FETCH_HEAD
fi
echo "mpp @ $(g rev-parse --short HEAD)"

B=$PWD/build/mpp
rm -rf "$B" out/mpp; mkdir -p "$B" out/mpp
cd "$B"
cmake -DCMAKE_TOOLCHAIN_FILE="$ROOT/$D/build/linux/aarch64/arm.linux.cross.cmake" \
    -DTOOLCHAIN=aarch64-linux-gnu- \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_FLAGS="${MPP_CFLAGS:--mcpu=cortex-a72.cortex-a53}" \
    -DCMAKE_CXX_FLAGS="${MPP_CFLAGS:--mcpu=cortex-a72.cortex-a53}" \
    -DBUILD_TEST=ON \
    "$ROOT/$D" >/dev/null
make -j"$(nproc)" >/dev/null
make install DESTDIR="$ROOT/out/mpp" >/dev/null
cd "$ROOT"
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" out/mpp
find out/mpp -type f \( -name '*.so*' -o -perm -u+x \) | sed 's#^#  #' | head -30
