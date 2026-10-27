#!/usr/bin/env bash
# Cross-build the open NPU stack for the NPU die (aarch64 Cortex-A35, glibc <= 2.29): TIM-VX, the TFLite VX delegate (TensorFlow Lite 2.6.0)
# and our server npu_server. Sources are pinned in sources.lock. Runs on the host and starts Docker itself (the toolchain image is built from
# docker/npu-xtool/Dockerfile: Debian 11, see there why). Needs the network (the sources and the dependencies that the TensorFlow Lite build fetches).
# The build links against the Vivante libraries of Rockchip's firmware, which it takes out of downloads/npu/npu_fw/boot.img (scripts/fetch-npu.sh);
# they are not copied anywhere.
#   scripts/build-npu-open.sh        -> out/npu-open/{npu_server,libvx_delegate.so,libtim-vx.so}
# Optimisation: -O3 and -mcpu=cortex-a35 (the CPU of the NPU die).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
. ./sources.lock
IMAGE=${NPU_XTOOL_IMAGE:-npu-xtool}
BOOT=${NPU_BOOT_IMG:-downloads/npu/npu_fw/boot.img}
[ -f "$BOOT" ] || { echo "$BOOT missing: run scripts/fetch-npu.sh first" >&2; exit 1; }
W=$ROOT/downloads/npu-open/work
O=$ROOT/out/npu-open
mkdir -p "$W/sdk/drivers" "$O" downloads/npu-open

docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" docker/npu-xtool

# 1. the Vivante libraries of the firmware (SDK 6.4.6) to link against
for f in libCLC.so libGAL.so libOpenVX.so libOpenVXU.so libVSC.so libArchModelSw.so libNNArchPerf.so; do
    python3 npu-open/tools/npu_ramdisk.py "$BOOT" --extract "/usr/lib/$f=$W/sdk/drivers/$f" >/dev/null
done

# 2. pinned sources
fetch_rev() {   # dir repo rev
    local d=$1
    if [ ! -d "$d/.git" ]; then mkdir -p "$d"; git init -q "$d"; git -C "$d" remote add origin "$2"; fi
    if [ "$(git -C "$d" rev-parse HEAD 2>/dev/null || true)" != "$3" ]; then
        git -C "$d" fetch -q --depth 1 origin "$3"
        git -C "$d" checkout -q --force FETCH_HEAD
    fi
    git -C "$d" clean -q -fdx
    git -C "$d" checkout -q --force .
}
fetch_rev "$W/TIM-VX" "$TIMVX_REPO" "$TIMVX_REV"
fetch_rev "$W/tflite-vx-delegate" "$TFLITE_VX_DELEGATE_REPO" "$TFLITE_VX_DELEGATE_REV"
TFTAR=downloads/npu-open/tensorflow-2.6.0.tar.gz
if ! echo "$TENSORFLOW_SHA256  $TFTAR" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL -o "$TFTAR" "$TENSORFLOW_URL"
    echo "$TENSORFLOW_SHA256  $TFTAR" | sha256sum -c - >/dev/null || { echo "TensorFlow tarball: checksum mismatch" >&2; exit 1; }
fi
if [ ! -f "$W/tensorflow-2.6.0/.patched" ]; then
    rm -rf "$W/tensorflow-2.6.0"; mkdir -p "$W/tensorflow-2.6.0"
    tar xzf "$TFTAR" -C "$W/tensorflow-2.6.0" --strip-components=1
    # the delegate needs its own patch in TensorFlow Lite (a customised NBG model)
    ( cd "$W/tensorflow-2.6.0" && patch -s -p1 < "$W/tflite-vx-delegate/patches/tflite/0001-Add-Customized-NBG-Tflite-Model-Support.patch" && touch .patched )
fi
rm -rf "$W/sdk/include"; cp -r "$W/TIM-VX/prebuilt-sdk/x86_64_linux/include" "$W/sdk/include"
# our changes to the delegate: build fixes for a current CMake, the compiled graph export, the server as a target
git -C "$W/tflite-vx-delegate" apply "$ROOT/npu-open/patches/0001-tflite-vx-delegate-build-fixes-and-nbg-export.patch"
rm -rf "$W/server"; cp -r "$ROOT/npu-open/server" "$W/server"

# 3. the build inside the toolchain container
docker run --rm -u "$(id -u):$(id -g)" -v "$W":/w -w /w "$IMAGE" bash -c '
set -e
mkdir -p build-timvx && cd build-timvx
cat > toolchain.cmake <<T
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a35")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a35 -static-libstdc++ -static-libgcc")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined -Wl,-rpath-link,/w/sdk/drivers")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined")
set(CMAKE_CXX_STANDARD_LIBRARIES "-ldl")
T
cmake ../TIM-VX -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain.cmake -DEXTERNAL_VIV_SDK=/w/sdk -DCMAKE_BUILD_TYPE=Release -DTIM_VX_ENABLE_TEST=OFF -DCMAKE_INSTALL_PREFIX=$PWD/install >/dev/null
make -j"$(nproc)" install >/dev/null
cd .. && mkdir -p build-delegate && cd build-delegate
cmake ../tflite-vx-delegate -DCMAKE_TOOLCHAIN_FILE=/w/build-timvx/toolchain.cmake -DFETCHCONTENT_SOURCE_DIR_TENSORFLOW=/w/tensorflow-2.6.0 \
  -DENABLE_NBG_SUPPORT=ON -DTIM_VX_INSTALL=/w/build-timvx/install -DTFLITE_ENABLE_XNNPACK=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
make -j"$(nproc)" vx_delegate npu_server
aarch64-linux-gnu-strip -o /w/out-libvx_delegate.so libvx_delegate.so
aarch64-linux-gnu-strip -o /w/out-npu_server npu_server/npu_server
aarch64-linux-gnu-strip -o /w/out-libtim-vx.so /w/build-timvx/install/lib/libtim-vx.so
'
install -m 0755 "$W/out-npu_server" "$O/npu_server"
install -m 0755 "$W/out-libvx_delegate.so" "$O/libvx_delegate.so"
install -m 0755 "$W/out-libtim-vx.so" "$O/libtim-vx.so"
# licences of what is built (shipped with the image)
mkdir -p "$O/licenses"
cp "$W/TIM-VX/LICENSE" "$O/licenses/TIM-VX-LICENSE"
cp "$W/tflite-vx-delegate/LICENSE" "$O/licenses/tflite-vx-delegate-LICENSE"
cp "$W/tensorflow-2.6.0/LICENSE" "$O/licenses/TensorFlow-LICENSE"
ls -l "$O"
