# TensorFlow Lite operators on the NPU

Which operators of a TFLite model run on the NPU of the Tinker Edge R (release 0.4.1: TensorFlow Lite 2.21, TIM-VX and the TFLite VX delegate), and, for comparison,
which ones Rockchip's RKNN-Toolkit 1.7.5 accepted.

* **NPU**: `yes` = the operator runs on the NPU with the right result (compared with TensorFlow on the CPU, tolerance 0.05); `CPU` = it runs on the CPU of the NPU die
  (with `device="auto"`; `device="npu"` refuses a model with such an operator and names it); `-` = not tested (no model could be made, or the test model
  also needs an operator that stays on the CPU).
* **RKNN**: `yes` = RKNN-Toolkit 1.7.5 accepted the model (it has no simulator, so its results were not checked); `no` = refused; `-` = not tested.
* One tiny float32 model per operator (input 1x8x8x4), 87 models, measured on one board on 2026-10-27 (NPU) and 2026-10-26 (RKNN).

68 of the 87 models run entirely on the NPU, none gives a wrong result. Comparisons, logical operators and `CAST` stay on the CPU on purpose: TIM-VX crashes on bool tensors with this driver.

| Operator | NPU | RKNN |
|---|---|---|
| `ABS` | yes | yes |
| `ADD` | yes | yes |
| `ADD_N` | yes | yes |
| `ARG_MAX` | yes | yes |
| `ARG_MIN` | yes | yes |
| `AVERAGE_POOL_2D` | yes | yes |
| `BATCH_MATMUL` | - | - |
| `BATCH_TO_SPACE_ND` | - | yes |
| `CAST` | CPU | no |
| `CEIL` | CPU | yes |
| `CONCATENATION` | yes | yes |
| `CONV_2D` | yes | yes |
| `COS` | CPU | yes |
| `DEPTHWISE_CONV_2D` | yes | yes |
| `DEPTH_TO_SPACE` | yes | yes |
| `DEQUANTIZE` | - | - |
| `DIV` | yes | - |
| `ELU` | yes | yes |
| `EQUAL` | CPU | no |
| `EXP` | yes | yes |
| `EXPAND_DIMS` | yes | - |
| `FILL` | yes | - |
| `FLOOR` | yes | yes |
| `FLOOR_DIV` | yes | yes |
| `FULLY_CONNECTED` | yes | yes |
| `GATHER` | yes | yes |
| `GATHER_ND` | yes | yes |
| `GREATER` | CPU | yes |
| `GREATER_EQUAL` | CPU | no |
| `HARD_SWISH` | yes | yes |
| `L2_NORMALIZATION` | yes | no |
| `L2_POOL_2D` | - | - |
| `LEAKY_RELU` | yes | yes |
| `LESS` | CPU | no |
| `LESS_EQUAL` | CPU | no |
| `LOCAL_RESPONSE_NORMALIZATION` | yes | yes |
| `LOG` | yes | yes |
| `LOGICAL_AND` | CPU | no |
| `LOGICAL_NOT` | - | - |
| `LOGICAL_OR` | CPU | no |
| `LOGISTIC` | yes | yes |
| `LOG_SOFTMAX` | CPU | yes |
| `LSTM` | - | - |
| `MAXIMUM` | yes | yes |
| `MAX_POOL_2D` | yes | yes |
| `MEAN` | yes | yes |
| `MINIMUM` | yes | yes |
| `MIRROR_PAD` | yes | yes |
| `MUL` | yes | yes |
| `NEG` | yes | yes |
| `NOT_EQUAL` | CPU | no |
| `ONE_HOT` | yes | no |
| `PACK` | yes | yes |
| `PAD` | yes | yes |
| `POW` | yes | yes |
| `PRELU` | - | - |
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
| `ROUND` | CPU | yes |
| `RSQRT` | yes | yes |
| `SELECT` | - | yes |
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
| `TILE` | CPU | yes |
| `TOPK_V2` | CPU | no |
| `TRANSPOSE` | yes | yes |
| `TRANSPOSE_CONV` | yes | yes |
| `UNIDIRECTIONAL_SEQUENCE_LSTM` | - | - |
| `UNPACK` | yes | yes |
| `ZEROS_LIKE` | yes | - |

Scripts and raw results (private notes): `rnd/verisilicon/models/gen_op_models.py`, `test_ops_041.py`, `rknn_check.py`, `ops/*.tsv`.
