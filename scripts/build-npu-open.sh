#!/usr/bin/env bash
# Cross-build the open NPU stack for the NPU die (aarch64 Cortex-A35, glibc <= 2.29): TIM-VX, the TFLite VX delegate built on LiteRT
# (google-ai-edge/LiteRT: TensorFlow Lite as tflite/, with XNNPACK for what stays on the CPU) and our server npu_server. Sources are pinned in
# sources.lock: LiteRT, the TensorFlow tree LiteRT pins for the few files it takes from it (LITERT_*), what its tflite/ builds with (LITERT_DEPS).
# Runs on the host and starts Docker itself (the toolchain image is built from docker/npu-xtool/Dockerfile: Debian 11, see there why). Needs the
# network to fetch the sources; CMake itself then runs disconnected. The build links against the Vivante libraries of Rockchip's firmware, which it
# takes out of downloads/npu/npu_fw/boot.img (scripts/fetch-npu.sh); they are not copied anywhere.
#   scripts/build-npu-open.sh        -> out/npu-open/{npu_server,libvx_delegate.so,libtim-vx.so,licenses/}
# Patches (npu-open/patches): 0001, 0003, 0004 to the delegate (0004: the build on LiteRT), 0002-litert to LiteRT's tflite/ (saturating NEON kernels).
# Optimisation: -O3 and -mcpu=cortex-a35 (the CPU of the NPU die); XNNPACK with its ARMv8.0 (NEON) kernels only.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
. ./sources.lock
IMAGE=${NPU_XTOOL_IMAGE:-npu-xtool}
BOOT=${NPU_BOOT_IMG:-downloads/npu/npu_fw/boot.img}
[ -f "$BOOT" ] || { echo "$BOOT missing: run scripts/fetch-npu.sh first" >&2; exit 1; }
W=$ROOT/downloads/npu-open/work-litert
O=$ROOT/out/npu-open
LR=LiteRT-$LITERT_VERSION
TFT=tensorflow-$LITERT_TF_COMMIT
mkdir -p "$W/sdk/drivers" "$W/deps" "$O" downloads/litert

# always from docker/npu-xtool (cached when it did not change): an older image under the same name would build with another toolchain
[ -n "${NPU_XTOOL_IMAGE:-}" ] || docker build -q -t "$IMAGE" docker/npu-xtool >/dev/null

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
fetch_tar() {   # dir url sha256 file
    if ! echo "$3  $4" | sha256sum -c - >/dev/null 2>&1; then
        curl -fsSL -o "$4" "$2"
        echo "$3  $4" | sha256sum -c - >/dev/null || { echo "$4: checksum mismatch" >&2; exit 1; }
    fi
    rm -rf "$1"; mkdir -p "$1"; tar xzf "$4" -C "$1" --strip-components=1
}
fetch_rev "$W/TIM-VX" "$TIMVX_REPO" "$TIMVX_REV"
fetch_rev "$W/TIM-VX-headers" "$TIMVX_REPO" "$TIMVX_SDK_HEADERS_REV"
fetch_rev "$W/tflite-vx-delegate" "$TFLITE_VX_DELEGATE_REPO" "$TFLITE_VX_DELEGATE_REV"
# the trees stay unpacked between builds (fetch_tar only when missing): our patch to LiteRT is applied once, below
[ -f "$W/$LR/tflite/CMakeLists.txt" ] || fetch_tar "$W/$LR" "$LITERT_URL" "$LITERT_SHA256" "downloads/litert/LiteRT-v$LITERT_VERSION.tar.gz"
[ -f "$W/$TFT/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc" ] || fetch_tar "$W/$TFT" "$LITERT_TF_URL" "$LITERT_TF_SHA256" "downloads/litert/$TFT.tar.gz"
echo "$LITERT_DEPS" | while read -r name repo rev; do if [ -n "$name" ]; then fetch_rev "$W/deps/$name" "$repo" "$rev"; fi; done
fetch_tar "$W/deps/fft2d" "$FFT2D_URL" "$FFT2D_SHA256" downloads/npu-open/OouraFFT-v1.0.tar.gz
# the headers of SDK 6.4.6 (see sources.lock): they describe what the driver of the firmware can do
rm -rf "$W/sdk/include"; cp -r "$W/TIM-VX-headers/prebuilt-sdk/x86_64_linux/include" "$W/sdk/include"
# our changes to the delegate: compiled graph export, int8 as uint8, folded constant Dequantize, the server as a target, TOPK_V2, LiteRT
for p in 0001-tflite-vx-delegate-nbg-export-int8-as-uint8-dequantize 0003-tflite-vx-delegate-topk-v2 0004-tflite-vx-delegate-build-on-litert; do
    git -C "$W/tflite-vx-delegate" apply "$ROOT/npu-open/patches/$p.patch"
done
# and to LiteRT's tflite/: saturating NEON int8 / uint8 ADD, SUB and MUL
P=$ROOT/npu-open/patches/0002-litert-saturating-neon-int8-uint8-add-mul.patch
if ! patch -d "$W/$LR" -p1 -R --dry-run -s -f < "$P" >/dev/null 2>&1; then patch -d "$W/$LR" -p1 -s -f < "$P"; fi
rm -rf "$W/server"; cp -r "$ROOT/npu-open/server" "$W/server"

# 3. the build inside the toolchain container
docker run --rm -u "$(id -u):$(id -g)" -v "$W":/w -w /w -e LR="$LR" -e TFT="$TFT" "$IMAGE" bash -c '
set -e
cat > /w/toolchain.cmake <<T
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
# flatc for the build machine (the cross build generates headers with it)
mkdir -p build-flatc && cd build-flatc
cmake ../$LR/tflite/tools/cmake/native_tools/flatbuffers -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_FLATBUFFERS=/w/deps/flatbuffers >/dev/null
make -j"$(nproc)" >/dev/null
cd .. && mkdir -p build-timvx && cd build-timvx
cmake ../TIM-VX -DCMAKE_TOOLCHAIN_FILE=/w/toolchain.cmake -DEXTERNAL_VIV_SDK=/w/sdk -DCMAKE_BUILD_TYPE=Release -DTIM_VX_ENABLE_TEST=OFF -DCMAKE_INSTALL_PREFIX=$PWD/install >/dev/null
make -j"$(nproc)" install >/dev/null
cd .. && mkdir -p build-delegate && cd build-delegate
D=/w/deps
cmake ../tflite-vx-delegate -DCMAKE_TOOLCHAIN_FILE=/w/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
  -DLITERT_SOURCE_DIR=/w/$LR -DTENSORFLOW_SOURCE_DIR=/w/$TFT \
  -DTIM_VX_INSTALL=/w/build-timvx/install -DTFLITE_HOST_TOOLS_DIR=/w/build-flatc \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_ABSEIL-CPP=$D/abseil-cpp -DFETCHCONTENT_SOURCE_DIR_EIGEN=$D/eigen -DFETCHCONTENT_SOURCE_DIR_FARMHASH=$D/farmhash \
  -DFETCHCONTENT_SOURCE_DIR_FFT2D=$D/fft2d -DFETCHCONTENT_SOURCE_DIR_FLATBUFFERS=$D/flatbuffers -DFETCHCONTENT_SOURCE_DIR_GEMMLOWP=$D/gemmlowp \
  -DFETCHCONTENT_SOURCE_DIR_ML_DTYPES=$D/ml_dtypes -DFETCHCONTENT_SOURCE_DIR_RUY=$D/ruy -DFETCHCONTENT_SOURCE_DIR_CPUINFO=$D/cpuinfo \
  -DFETCHCONTENT_SOURCE_DIR_PROTOBUF=$D/protobuf -DFETCHCONTENT_SOURCE_DIR_XNNPACK=$D/xnnpack -DFETCHCONTENT_SOURCE_DIR_FP16_HEADERS=$D/fp16 \
  -DCPUINFO_SOURCE_DIR=$D/cpuinfo -DFP16_SOURCE_DIR=$D/fp16 -DPTHREADPOOL_SOURCE_DIR=$D/pthreadpool -DFXDIV_SOURCE_DIR=$D/FXdiv \
  -DTFLITE_ENABLE_XNNPACK=ON -DTFLITE_ENABLE_GPU=OFF -DTFLITE_ENABLE_NNAPI=OFF \
  -DXNNPACK_ENABLE_ARM_FP16_SCALAR=OFF -DXNNPACK_ENABLE_ARM_FP16_VECTOR=OFF -DXNNPACK_ENABLE_ARM_BF16=OFF -DXNNPACK_ENABLE_ARM_DOTPROD=OFF \
  -DXNNPACK_ENABLE_ARM_I8MM=OFF -DXNNPACK_ENABLE_ARM_SME=OFF -DXNNPACK_ENABLE_ARM_SME2=OFF -DXNNPACK_ENABLE_KLEIDIAI=OFF > /w/cmake-delegate.log 2>&1 || { tail -40 /w/cmake-delegate.log; exit 1; }
make -j"$(nproc)" vx_delegate npu_server
aarch64-linux-gnu-strip -o /w/out-libvx_delegate.so libvx_delegate.so
aarch64-linux-gnu-strip -o /w/out-npu_server npu_server/npu_server
aarch64-linux-gnu-strip -o /w/out-libtim-vx.so /w/build-timvx/install/lib/libtim-vx.so
'
install -m 0755 "$W/out-npu_server" "$O/npu_server"
install -m 0755 "$W/out-libvx_delegate.so" "$O/libvx_delegate.so"
install -m 0755 "$W/out-libtim-vx.so" "$O/libtim-vx.so"
# licences of what is built into these binaries (shipped with the image)
L=$O/licenses
rm -rf "$L"; mkdir -p "$L"
cp "$W/TIM-VX/LICENSE" "$L/TIM-VX-LICENSE"
cp "$W/tflite-vx-delegate/LICENSE" "$L/tflite-vx-delegate-LICENSE"
cp "$W/$LR/LICENSE" "$L/LiteRT-LICENSE"
cp "$W/$TFT/LICENSE" "$L/TensorFlow-LICENSE"
D=$W/deps
cp "$D/abseil-cpp/LICENSE" "$L/abseil-cpp-LICENSE"
cp "$D/eigen/COPYING.MPL2" "$L/Eigen-COPYING.MPL2"; cp "$D/eigen/COPYING.README" "$L/Eigen-COPYING.README"
cp "$D/farmhash/COPYING" "$L/farmhash-COPYING"
cp "$D/fft2d/readme2d.txt" "$L/fft2d-readme2d.txt"
cp "$D/flatbuffers/LICENSE" "$L/flatbuffers-LICENSE"
cp "$D/gemmlowp/LICENSE" "$L/gemmlowp-LICENSE"
cp "$D/ml_dtypes/LICENSE" "$L/ml_dtypes-LICENSE"
cp "$D/ruy/LICENSE" "$L/ruy-LICENSE"
cp "$D/cpuinfo/LICENSE" "$L/cpuinfo-LICENSE"
cp "$D/fp16/LICENSE" "$L/FP16-LICENSE"
cp "$D/xnnpack/LICENSE" "$L/XNNPACK-LICENSE"
cp "$D/pthreadpool/LICENSE" "$L/pthreadpool-LICENSE"
cp "$D/FXdiv/LICENSE" "$L/FXdiv-LICENSE"
ls -l "$O"
