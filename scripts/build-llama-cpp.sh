#!/usr/bin/env bash
# Build the latest published llama.cpp release on the Tinker Edge R (Debian arm64).
# Dependencies: sudo apt install build-essential cmake ninja-build git curl ca-certificates \
#     python3 libssl-dev libopenblas-dev ocl-icd-opencl-dev
# Run as an ordinary user: ./scripts/build-llama-cpp.sh
set -euo pipefail

cross=()
if [[ ${1:-} == --image ]]; then
    # This mode runs inside the disposable Debian multiarch builder.
    sed -i 's|http://deb.debian.org|https://deb.debian.org|g' /etc/apt/sources.list.d/debian.sources
    apt-get update
    apt-get install -y --no-install-recommends ninja-build ocl-icd-opencl-dev:arm64 libopenblas-dev:arm64 libssl-dev:arm64
    cross=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
           -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
           -DOpenCL_LIBRARY=/usr/lib/aarch64-linux-gnu/libOpenCL.so
           -DOPENSSL_SSL_LIBRARY=/usr/lib/aarch64-linux-gnu/libssl.so
           -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/aarch64-linux-gnu/libcrypto.so
           -DBLAS_LIBRARIES=/usr/lib/aarch64-linux-gnu/libopenblas.so)
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
    export LLAMA_CPP_RELEASE=pinned
elif [[ $(uname -m) != aarch64 ]]; then
    echo 'Run this script on the Tinker Edge R (arm64).' >&2
    exit 1
fi

repo=https://github.com/ggml-org/llama.cpp.git
root=$(cd "$(dirname "$0")/.." && pwd)
source "$root/sources.lock"
release=${LLAMA_CPP_RELEASE:-latest}
if [[ $release == latest ]]; then
    release=$(curl -fsSL --retry 3 https://api.github.com/repos/ggml-org/llama.cpp/releases/latest | \
    python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])')
elif [[ $release == pinned ]]; then
    release=$LLAMA_CPP_PATCH_TAG
fi
git check-ref-format "refs/tags/$release"
if [[ ! $release =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
    echo "Unexpected release tag: $release" >&2
    exit 1
fi

work="$PWD/out/llama-cpp/$release"
[[ ${1:-} != --image ]] || work+=-image
mkdir -p "$work"
if [[ ! -d $work/source/.git ]]; then
    git clone --depth 1 --branch "$release" "$repo" "$work/source"
fi
revision=$(git -C "$work/source" rev-parse HEAD)
[[ $revision == "$(git -C "$work/source" rev-parse "refs/tags/$release^{commit}")" ]] || exit 1
if [[ $release == "$LLAMA_CPP_PATCH_TAG" && $revision != "$LLAMA_CPP_PATCH_REV" ]]; then
    echo "Patch base mismatch: $revision" >&2
    exit 1
fi
echo "Building llama.cpp $release ($revision)"

patch_dir="$root/patches/llama-cpp"
if ! git -C "$work/source" apply --reverse --check "$patch_dir/mali-t860.patch" 2>/dev/null; then
    git -C "$work/source" apply "$patch_dir/mali-t860.patch"
fi
{ echo 'R"CLC('; cat "$patch_dir/mul_mat_q4_k.cl"; echo ')CLC"'; } > "$work/source/ggml/src/ggml-opencl/edge-mali.cl.inc"

cmake -S "$work/source" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -mcpu=cortex-a72.cortex-a53' \
    -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -mcpu=cortex-a72.cortex-a53' \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF -DGGML_LTO=ON -DGGML_CPU_REPACK=ON \
    -DGGML_OPENMP=ON -DGGML_BLAS=ON -DGGML_BLAS_VENDOR=OpenBLAS \
    -DGGML_OPENCL=ON -DGGML_OPENCL_EMBED_KERNELS=ON \
    -DGGML_OPENCL_USE_ADRENO_KERNELS=OFF -DGGML_OPENCL_USE_ADRENO_BIN_KERNELS=OFF \
    -DGGML_VULKAN=OFF \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON \
    -DLLAMA_BUILD_SERVER=ON -DLLAMA_OPENSSL=ON "${cross[@]}"

# Two jobs by default to keep compiler and LTO memory usage within 4 GB RAM.
cmake --build "$work/build" --parallel "${JOBS:-2}" --target llama-cli llama-server llama-bench
bin="$work/build/bin"
if [[ ${1:-} == --image ]]; then
    dest="$root/out/llama-package"
    mkdir -p "$dest/usr/bin" "$dest/usr/share/doc/llama.cpp"
    for tool in llama-cli llama-server llama-bench; do
        aarch64-linux-gnu-strip -o "$dest/usr/bin/$tool" "$bin/$tool"
    done
    cp "$work/source/LICENSE" "$dest/usr/share/doc/llama.cpp/copyright"
    echo "$release $revision" > "$dest/usr/share/doc/llama.cpp/version"
    exit 0
fi
"$bin/llama-cli" --version
echo "Binaries: $bin"
echo "CPU: $bin/llama-cli -hf LiquidAI/LFM3-1.7B-GGUF:Q4_K_M -ngl 0 -t 6 -c 2048 --temp 0.1 --top-k 50 --repeat-penalty 1.1"
echo "GPU detection: RUSTICL_ENABLE=panfrost $bin/llama-cli --list-devices"
