# TensorFlow Lite operators on the NPU

## Quantized operators on LiteRT 2.4.0

Measured on the RK3399Pro on 2026-11-27. Each fixture runs first with
`device="cpu"` (LiteRT on the NPU die), then uncached with `device="npu"`.
The test checks the placement of every operator and compares every output
element on the same eight inputs. The 256-element unary fixtures exercise
all input codes. These results apply to the specified quantization parameters.

| Operator | INT8 and UINT8 result on the full NPU |
|---|---|
| `BROADCAST_TO` | Exact for static [1,8,1,4] → [1,8,8,4]. Stock 0.6 failed its first run when the broadcast was the graph output. The server materializes same-rank broadcasts with concatenations, up to 64 copies per expanded axis. |
| `PADV2` | Exact with a constant fill equal to the quantized zero point. All 12 such nodes in GoogLeNet run as PAD. Dynamic and nonzero fills still require CPU. |
| `DEQUANTIZE` | Exact for an input scale of 1/8. |
| `RELU_N1_TO_1`, `HARD_SWISH` | Exact for input and output scales of 1/8. |
| `QUANTIZE` | Exact for same-type requantization at input/output scale ratio 1/2, including saturation at extreme zero points. Previously 524 of 2048 results differed by one code per dtype. Other ratios need separate checks. |
| `LOGISTIC`, `MUL` | All nodes delegated. Both types still differ from CPU by up to one output code on these fixtures. |

Generate the cases on the build host and copy the generated models and the
comparison script to the board:

```sh
python3 tests/npu/dev/make_quantized_ops.py \
  downloads/npu-open/work-litert/build-flatc-LiteRT-2.4.0/flatbuffers-flatc/bin/flatc \
  downloads/npu-open/work-litert/LiteRT-2.4.0/tflite/converter/schema/schema.fbs out/quantized-ops
# On the board, with the fixtures in quantized-ops/
python3 compare_quantized.py quantized-ops/*.tflite
```

A numerical mismatch makes the command exit with status 1. Use the same runner
with a complete model; `--no-split` isolates the effect of per-channel weight
regrouping. Timings are median warm end-to-end latency, including the host/USB
round trip. Random inputs are diagnostic; task accuracy needs representative data.

### Whole quantized models

Eight identical diagnostic inputs per model, warm median in ms including USB.
The CPU column uses LiteRT on the NPU die. Output differences are in integer
codes. The MobileNet V2 files are separate model exports; compare each to its
own CPU run.

| Model | CPU ms | Full NPU ms | Largest output difference |
|---|---:|---:|---:|
| MobileNet V2 UINT8 | 115.29 | 10.22 | 5 |
| MobileNet V2 INT8, grouped weights | 112.02 | 30.57 | 101 |
| MobileNet V2 INT8, original per-channel weights | 111.94 | 137.31 | 18 |
| MobileNet V3 large INT8 | 99.20 | 75.95 | 22 |
| EfficientNet-B0 INT8 | 364.76 | 748.82 | 6 |

On the project's dog photograph (224×224 RGB, normalized to [-1,1]), the CPU
and full NPU both selected class 156 with both MobileNet V2 exports. For UINT8
the top five contained the same classes in a different order. For INT8 the
first three matched, and the last two classes differed. Maximum differences
on this image were four codes (UINT8) and three (INT8).

The small self-test INT8 network matched the CPU on all eight inputs with the
original per-channel weights. With grouping enabled its maximum difference was
two codes. On isolated layers from the same model, the first grouped convolution
already differed by up to seven codes. The UINT8 self-test network differed by
one code with either splitting policy.

### Operator coverage of the model set

Count each distinct builtin opcode once across the original FlatBuffer subgraphs,
before server rewrites. The project catalogue contains 99 types: 97 in the
historical survey below, plus BROADCAST_TO and PADV2. Of those, 93 have at least
one board-tested NPU configuration (88 marked yes below, plus QUANTIZE,
DEQUANTIZE, RELU_N1_TO_1, BROADCAST_TO and PADV2).

| Quantized model files | Distinct original types | Share of catalogue | Share of board-supported types |
|---|---:|---:|---:|
| ResNet18, SqueezeNet 1.1 and GoogLeNet | 13 | 13/99 (13.1%) | 13/93 (14.0%) |
| Above plus MobileNet V2 UINT8 and INT8, MobileNet V3 large and EfficientNet-B0 | 18 | 18/99 (18.2%) | 18/93 (19.4%) |
| Above plus EfficientDet-Lite0 INT8 | 19 | 19/99 (19.2%) | 19/93 (20.4%) |

EfficientDet ran fully on the NPU (290.43 ms, CPU 485.35 ms). Its two raw float outputs differed by up to 1.09013; detection mAP was not measured.
EfficientDet adds RESIZE_NEAREST_NEIGHBOR. Across the eight original graphs,
534 of 963 nodes (55.5%) are CONV_2D or DEPTHWISE_CONV_2D. Quantized classifiers alone exercise
convolution, pooling, residual ADD, concatenation, and fully connected layers.
GATHER, BATCH_MATMUL, TRANSPOSE_CONV, and RESIZE_BILINEAR are examples absent
from all eight files. Standalone operator fixtures and ConvNeXt W8A16 are outside
this model count. Fused activations are part of their parent operator.

## Earlier operator survey

Which operators of a TFLite model run on the NPU of the Tinker Edge R (LiteRT 2.3.2, TIM-VX and the TFLite VX delegate, with the rewrites of our server; main branch
after release 0.4.1, measured on 2026-10-28), and, for comparison,
which ones Rockchip's RKNN-Toolkit 1.7.5 accepted.

* **NPU**: `yes` = the operator runs on the NPU with the right result (compared with TensorFlow on the CPU, tolerance 0.05); `CPU` = it runs on the CPU of the NPU die
  (with `device="auto"`; `device="npu"` refuses a model with such an operator and names it); `-` = not tested (no model could be made, or the test model
  also needs an operator that stays on the CPU).
* **RKNN**: `yes` = RKNN-Toolkit 1.7.5 accepted the model (it has no simulator, so its results were not checked); `no` = refused; `-` = not tested.
* One tiny float32 model per operator (input 1x8x8x4), 87 models, measured on one board on 2026-10-28 (NPU) and 2026-10-26 (RKNN).

All 87 models run entirely on the NPU, none gives a wrong result. `PRELU` (its generated model had become `RELU`) was checked with its own models: alpha per channel and one alpha, float32 / int8 / uint8, all on the NPU and exact. Our server rewrites operators into ones the NPU runs (`npu-open/server/rewrite_ops.h`):

| Operator | Rewritten as |
|---|---|
| `SLICE` | `STRIDED_SLICE` |
| `TILE` | `CONCATENATION` of copies |
| `LOG_SOFTMAX` | `SOFTMAX` + `LOG` |
| `CEIL` | `NEG(FLOOR(NEG(x)))` |
| `COS` | `SIN(x + pi / 2)` |
| `ROUND` (half to even) | `FLOOR(x + 0.5)`, minus 1 where x + 0.5 is a whole odd number |
| `CEIL`, `COS`, `ROUND` of int8 / uint8 | `DEQUANTIZE`, the float form, `QUANTIZE` |
| `CAST` float -> int32 -> float (truncation) | `MAXIMUM(FLOOR(x), 0) + MINIMUM(CEIL(x), 0)` |
| comparisons (`GREATER`, `LESS`, `EQUAL`, ...) | bool becomes 0 / 1 of the input type: `SUB` + `MUL` (float: + `MAXIMUM` / `MINIMUM` to [0, 1]; int8 / uint8: the requantisation saturates to 0 / 1) |
| `LOGICAL_AND` / `OR` / `NOT` | `MUL` / `MAXIMUM` / 1 - x |
| `SELECT` | b + c * (a - b) |
| `CAST` of bool | `RESHAPE` (float) or `DEQUANTIZE` (int8 / uint8) |
| `PRELU` with one alpha | the same `PRELU`, alpha repeated along the channels (TIM-VX crashes on a single int8 / uint8 alpha) |

`TOPK_V2` is not rewritten: our delegate patch maps it to TIM-VX `Topk` (`npu-open/patches/0003`). TIM-VX crashes on bool tensors with this driver; a bool tensor that cannot be rewritten (a model output, or used by an operator not in this table) stays on the CPU.
Int8 and uint8 comparisons were also checked with quantised models (`rnd/verisilicon/models/q8/`): the NPU gives the exact result of the dequantised values; TensorFlow Lite on
the CPU differs at values closer than about 1/256 (its comparison of two different scales is that coarse).

| Operator | NPU | RKNN |
|---|---|---|
| `ABS` | yes | yes |
| `ADD` | yes | yes |
| `ADD_N` | yes | yes |
| `ARG_MAX` | yes | yes |
| `ARG_MIN` | yes | yes |
| `AVERAGE_POOL_2D` | yes | yes |
| `BATCH_MATMUL` | yes | - |
| `BATCH_TO_SPACE_ND` | yes | yes |
| `CAST` | yes | no |
| `CEIL` | yes | yes |
| `CONCATENATION` | yes | yes |
| `CONV_2D` | yes | yes |
| `COS` | yes | yes |
| `DEPTHWISE_CONV_2D` | yes | yes |
| `DEPTH_TO_SPACE` | yes | yes |
| `DEQUANTIZE` | - | - |
| `DIV` | yes | - |
| `ELU` | yes | yes |
| `EQUAL` | yes | no |
| `EXP` | yes | yes |
| `EXPAND_DIMS` | yes | - |
| `FILL` | yes | - |
| `FLOOR` | yes | yes |
| `FLOOR_DIV` | yes | yes |
| `FULLY_CONNECTED` | yes | yes |
| `GATHER` | yes | yes |
| `GATHER_ND` | yes | yes |
| `GREATER` | yes | yes |
| `GREATER_EQUAL` | yes | no |
| `HARD_SWISH` | yes | yes |
| `L2_NORMALIZATION` | yes | no |
| `L2_POOL_2D` | - | - |
| `LEAKY_RELU` | yes | yes |
| `LESS` | yes | no |
| `LESS_EQUAL` | yes | no |
| `LOCAL_RESPONSE_NORMALIZATION` | yes | yes |
| `LOG` | yes | yes |
| `LOGICAL_AND` | yes | no |
| `LOGICAL_NOT` | yes | - |
| `LOGICAL_OR` | yes | no |
| `LOGISTIC` | yes | yes |
| `LOG_SOFTMAX` | yes | yes |
| `LSTM` | - | - |
| `MAXIMUM` | yes | yes |
| `MAX_POOL_2D` | yes | yes |
| `MEAN` | yes | yes |
| `MINIMUM` | yes | yes |
| `MIRROR_PAD` | yes | yes |
| `MUL` | yes | yes |
| `NEG` | yes | yes |
| `NOT_EQUAL` | yes | no |
| `ONE_HOT` | yes | no |
| `PACK` | yes | yes |
| `PAD` | yes | yes |
| `POW` | yes | yes |
| `PRELU` | yes | - |
| `QUANTIZE` | - | - |
| `RANGE` | yes | - |
| `REDUCE_ANY` | - | - |
| `REDUCE_MAX` | yes | yes |
| `REDUCE_MIN` | yes | yes |
| `REDUCE_PROD` | yes | no |
| `RELU` | yes | yes |
| `RELU6` | yes | - |
| `RELU_N1_TO_1` | - | - |
| `RESHAPE` | yes | yes |
| `RESIZE_BILINEAR` | yes | yes |
| `RESIZE_NEAREST_NEIGHBOR` | yes | yes |
| `REVERSE_V2` | yes | no |
| `ROUND` | yes | yes |
| `RSQRT` | yes | yes |
| `SELECT` | yes | yes |
| `SELECT_V2` | - | - |
| `SHAPE` | yes | - |
| `SIN` | yes | yes |
| `SLICE` | yes | yes |
| `SOFTMAX` | yes | yes |
| `SPACE_TO_BATCH_ND` | yes | yes |
| `SPACE_TO_DEPTH` | yes | yes |
| `SPLIT` | yes | yes |
| `SPLIT_V` | yes | no |
| `SQRT` | yes | yes |
| `SQUARE` | yes | yes |
| `SQUARED_DIFFERENCE` | yes | no |
| `SQUEEZE` | yes | - |
| `STRIDED_SLICE` | yes | yes |
| `SUB` | yes | yes |
| `SUM` | yes | yes |
| `SVDF` | - | - |
| `TANH` | yes | yes |
| `TILE` | yes | yes |
| `TOPK_V2` | yes | no |
| `TRANSPOSE` | yes | yes |
| `TRANSPOSE_CONV` | yes | yes |
| `UNIDIRECTIONAL_SEQUENCE_LSTM` | - | - |
| `UNPACK` | yes | yes |
| `ZEROS_LIKE` | yes | - |

Scripts and raw results (private notes): `rnd/verisilicon/models/gen_op_models.py`, `test_ops_041.py`, `rknn_check.py`, `ops/*.tsv`.

Found on the way: the NEON int8 / uint8 `ADD`, `SUB` and `MUL` kernels of TensorFlow Lite 2.21 and LiteRT 2.3.2 (used on the CPU of the NPU die when XNNPACK does not take an operator)
add the output zero point in 16 bits without saturation: a large result wraps around to the opposite extreme (an int8 `MUL` with a requantisation multiplier of 4 gave 127
instead of -128). Our build saturates there (`npu-open/patches/0002-litert`), as the reference kernels do.
