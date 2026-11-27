#!/usr/bin/env bash
# Build the latest published llama.cpp release on the Tinker Edge R (Debian arm64).
# Dependencies: sudo apt install build-essential cmake ninja-build git curl ca-certificates \
#     python3 libssl-dev libopenblas-dev ocl-icd-opencl-dev
# Run as an ordinary user: ./scripts/build-llama-cpp.sh
set -euo pipefail

if [[ $(uname -m) != aarch64 ]]; then
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
    -DLLAMA_BUILD_SERVER=ON -DLLAMA_OPENSSL=ON

# Two jobs by default to keep compiler and LTO memory usage within 4 GB RAM.
cmake --build "$work/build" --parallel "${JOBS:-2}" --target llama-cli llama-server llama-bench
bin="$work/build/bin"
"$bin/llama-cli" --version
echo "Binaries: $bin"
echo "CPU: $bin/llama-cli -hf LiquidAI/LFM3-1.7B-GGUF:Q4_K_M -ngl 0 -t 6 -c 2048 --temp 0.1 --top-k 50 --repeat-penalty 1.1"
echo "GPU detection: RUSTICL_ENABLE=panfrost $bin/llama-cli --list-devices"
