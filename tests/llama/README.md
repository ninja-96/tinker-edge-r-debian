# Mali-T860 inference checks

Board: Tinker Edge R, Rusticl/Panfrost, llama.cpp v1.1.0 at
`171527522610996f5f0acf12334e185156ca8f44`. Set `RUSTICL_ENABLE=panfrost`.
All text models below use Q4_K_M.

| Model | Board result |
| --- | --- |
| LiquidAI/LFM3-1.7B-GGUF | Generated four tokens with the initial Q4_K backend and CPU KV cache. |
| LiquidAI/LFM3-VL-2B-GGUF | With the Q8_0 projector, CPU and one GPU layer (CPU projector/cache) answered “Red”. The cache-enabled backend with all layers requested identified red in a longer answer, failed the one-word check, and reported unsupported vision operators. |
| lmstudio-community/gemma-4-E2B-it-GGUF | The fit estimator assigned 1389 MiB of weights to OpenCL and 2179 MiB to host. |
| lmstudio-community/gemma-4-E4B-it-GGUF | The fit estimator assigned 2813 MiB of weights to OpenCL and 2784 MiB to host. The estimated GPU portion alone exceeds the advertised 1922 MiB. |

The Gemma checks used actual model files and `llama-fit-params --fit-print on -ngl 99 -c 512 -b 32 -ub 32`. They estimate allocation; neither qualifies generation or quality. On VL the vision graph includes depthwise convolution, normalization, RoPE and softmax operations outside the current GPU implementation. Requesting all layers does not guarantee GPU execution for every operation.

Run the upstream CPU comparison on the board after building the `test-backend-ops` target with `LLAMA_BUILD_TESTS=ON`:

```sh
test-backend-ops test -b GPUOpenCL -o FILL
test-backend-ops test -b GPUOpenCL -o CPY,SET_ROWS -p 'type_src=f(32|16)'
test-backend-ops test -b GPUOpenCL -o MUL_MAT -p 'type_a=(q4_K|q8_0|q6_K),type_b=f32,m=16,n=(1|8),k=(256|4096),'
```

The selected hardware comparisons passed 10 fill, 253 copy/indexed-write and 16 matrix multiplication cases. Unsupported layouts are skipped by the upstream test runner.

For the VL smoke test start `llama-server` with `-m LFM3-VL-2B-Q4_K_M.gguf --mmproj mmproj-LFM3-VL-2B-Q8_0.gguf -np 1 -c 512` and run `python3 tests/llama/vision_smoke.py`. Model and projector revision: `1add221a5ea6c047fcf1eaaf2d247939d7cf8849`. Run once with CPU placement and again with the intended GPU settings. Check the server logs for fallback separately.
