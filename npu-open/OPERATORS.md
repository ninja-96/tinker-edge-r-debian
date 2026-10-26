# TensorFlow Lite operators: RKNN-Toolkit 1.7.5 against the open stack (TIM-VX + tflite-vx-delegate, TensorFlow Lite 2.6.0)

Measured on 2026-10-26 on one Tinker Edge R (RK3399Pro, Vivante driver 6.4.6). The question: **which operators of a TFLite model work with the open NPU stack and which with RKNN**,
so that the RKNN path can be dropped when the open stack has parity.

## Method

* The documented lists: RKNN from `RKNN_OP_Support_V1.7.5.md` (Rockchip), the open stack from the operator table in `op_map.cc` of the delegate (tag `tflite_v2.6.0`).
  The documents are not the whole truth, so every operator was also tested.
* For each operator one tiny float32 TFLite model (input 1x8x8x4, TensorFlow 2.2: the schema RKNN-Toolkit 1.7.5 reads), 87 models in all.
  The converter sometimes rewrites an operator (`DIV` became `MUL`, `RELU6` became `MUL`, `SQUEEZE` and `EXPAND_DIMS` became `RESHAPE`) or adds a helper (`CAST` after a comparison):
  such an operator counts as **not tested**, and a failure is blamed on an operator only when it is the only suspect.
* **RKNN:** `load_tflite()` and `build(do_quantization=False)` of the Toolkit. The PC simulator no longer exists after Toolkit 1.7.3, so *numbers were not checked*:
  "works" means that the Toolkit accepts the model; it does not say whether the operator then runs on the NPU or on a CPU.
* **Open stack:** the model is loaded by the server with `NPU_STRICT=1`: the load is refused if the delegate left any operator to the CPU, and the names of those operators are returned.
  So "works" means *the operator runs on the NPU*, and the result was compared with the CPU of TFLite (tolerance 0.05).
* Float32 models were used for coverage. Per-operator behaviour of the int8 / uint8 paths is not covered here (the whole-network results are in `README.md`).

## Result

| | Operators |
|---|---|
| work in both | 44 |
| accepted by RKNN, **not on the NPU** in the open stack (CPU fallback of TFLite) | CEIL, COS, FLOOR, FLOOR_DIV, GREATER, LOG_SOFTMAX, MIRROR_PAD, ROUND, SLICE, SUM, TILE, UNPACK |
| accepted by RKNN, wrong or crashing in the open stack | GATHER_ND and RESIZE_NEAREST_NEIGHBOR (wrong numbers); FULLY_CONNECTED and TRANSPOSE_CONV in a *float* graph (TIM-VX aborts in layout inference; the int8 `FULLY_CONNECTED` of ResNet50 runs fine, so this is a float-graph problem; `TRANSPOSE_CONV` was not tested in int8) |
| work only in the open stack | L2_NORMALIZATION, REDUCE_PROD (RKNN refuses them although L2_NORMALIZATION is in its list) |
| refused by both | ONE_HOT, REVERSE_V2, SPLIT_V (in the RKNN list, but refused), SQUARED_DIFFERENCE, TOPK_V2, CAST |
| not tested | the operator vanished in the converter, or no model could be made: BATCH_MATMUL, DEQUANTIZE, DIV, EXPAND_DIMS, L2_POOL_2D, LSTM, PRELU, QUANTIZE, REDUCE_ANY, RELU6, RELU_N1_TO_1, SELECT_V2, SHAPE, SQUEEZE, SVDF, UNIDIRECTIONAL_SEQUENCE_LSTM, ... |

What it means:

1. **The common core is large and is what vision networks use:** convolutions, depthwise, pooling, fully connected, ADD / MUL / SUB, concatenation, reshape / transpose / pad, softmax, logistic / tanh, resize bilinear,
   mean / max / min reductions, gather, strided slice, space-to-depth. Whole networks confirm it (MobileNet V2, ResNet50, Inception V3 run completely on the NPU with the open stack).
2. **The open stack is better than its documents:** the delegate runs ABS, ELU, EXP, LOG, SIN, SQUARE, MAXIMUM, MINIMUM, MEAN, PACK, HARD_SWISH, ADD_N and others that the RKNN list does not name
   (RKNN-Toolkit accepted these too, so its list is just as incomplete).
3. **Real gaps of the open stack against RKNN** (an operator that RKNN takes and the NPU does not run through the delegate): `SUM`, `TILE`, `UNPACK`, `SLICE`, `FLOOR`, `FLOOR_DIV`, `LOG_SOFTMAX`, `MIRROR_PAD`,
   comparisons (`GREATER` ...), and the recurrent operators `LSTM` / `SVDF` (not in the delegate at all). A model that needs them still runs: those operators go to the CPU of the NPU die (two Cortex-A35, slow, and the
   tensors leave the NPU), or they must be rewritten when the model is made (`SUM` as `MEAN` times N, `SLICE` as `STRIDED_SLICE`, `LOG_SOFTMAX` as `SOFTMAX` + `LOG`).
4. **Bugs to fix in the open stack** before RKNN can go: `GATHER_ND`, `RESIZE_NEAREST_NEIGHBOR` (wrong results), and TIM-VX aborts for float `FULLY_CONNECTED` / `TRANSPOSE_CONV`.
5. The RKNN list is not reliable either: `SPLIT_V` and `L2_NORMALIZATION` are listed and refused by the Toolkit 1.7.5.
6. Not covered: LSTM / GRU style recurrent models, dynamic shapes (`SHAPE`, `PACK` of shapes: Keras MobileNet V1 is refused by the delegate for this reason), operators of newer TensorFlow than 2.6.

## Per operator

`RKNN list` = named in the Rockchip document; `RKNN tested` = accepted by the Toolkit 1.7.5; `VX code` = has a mapper in `op_map.cc`; `VX tested` = what the board did.

| TFLite operator | RKNN list | RKNN tested | VX code | VX tested |
|---|---|---|---|---|
| `ABS` | no | works | yes | works |
| `ADD` | yes | works | yes | works |
| `ADD_N` | no | works | yes | works |
| `ARG_MAX` | yes | works | yes | works |
| `ARG_MIN` | yes | works | yes | works |
| `AVERAGE_POOL_2D` | yes | works | yes | works |
| `BATCH_MATMUL` | no | not tested | no | not tested |
| `BATCH_TO_SPACE_ND` | yes | works | yes | not tested |
| `CAST` | no | refused | no | CPU only |
| `CEIL` | no | works | no | CPU only |
| `CONCATENATION` | yes | works | yes | works |
| `CONV_2D` | yes | works | yes | works |
| `COS` | no | works | no | CPU only |
| `DEPTHWISE_CONV_2D` | yes | works | yes | works |
| `DEPTH_TO_SPACE` | yes | works | yes | works |
| `DEQUANTIZE` | yes | not tested | yes | not tested |
| `DIV` | yes | not tested | yes | not tested |
| `ELU` | no | works | yes | works |
| `EQUAL` | no | refused (with CAST) | no | CPU only |
| `EXP` | no | works | yes | works |
| `EXPAND_DIMS` | no | not tested | yes | not tested |
| `FILL` | no | not tested | no | not tested |
| `FLOOR` | yes | works | no | CPU only |
| `FLOOR_DIV` | yes | works | no | CPU only |
| `FULLY_CONNECTED` | yes | works | yes | crash |
| `GATHER` | yes | works | yes | works |
| `GATHER_ND` | no | works | yes | wrong result |
| `GREATER` | yes | works | no | CPU only |
| `GREATER_EQUAL` | yes | refused (with CAST) | no | CPU only |
| `HARD_SWISH` | no | works | yes | works |
| `L2_NORMALIZATION` | yes | refused | yes | works |
| `L2_POOL_2D` | yes | not tested | no | not tested |
| `LEAKY_RELU` | yes | works | yes | works |
| `LESS` | yes | refused (with CAST) | no | CPU only |
| `LESS_EQUAL` | yes | refused (with CAST) | no | CPU only |
| `LOCAL_RESPONSE_NORMALIZATION` | yes | works | yes | works |
| `LOG` | no | works | yes | works |
| `LOGICAL_AND` | no | refused (with CAST) | yes | not tested |
| `LOGICAL_NOT` | no | not tested | yes | not tested |
| `LOGICAL_OR` | no | refused (with CAST) | yes | not tested |
| `LOGISTIC` | yes | works | yes | works |
| `LOG_SOFTMAX` | yes | works | no | CPU only |
| `LSTM` | yes | not tested | no | not tested |
| `MAXIMUM` | no | works | yes | works |
| `MAX_POOL_2D` | yes | works | yes | works |
| `MEAN` | no | works | yes | works |
| `MINIMUM` | no | works | yes | works |
| `MIRROR_PAD` | yes | works | no | CPU only |
| `MUL` | yes | works | yes | works |
| `NEG` | yes | works | yes | works |
| `NOT_EQUAL` | yes | refused (with CAST) | no | CPU only |
| `ONE_HOT` | no | refused | no | CPU only |
| `PACK` | no | works | yes | works |
| `PAD` | yes | works | yes | works |
| `POW` | yes | works | yes | works |
| `PRELU` | yes | not tested | yes | not tested |
| `QUANTIZE` | no | not tested | yes | not tested |
| `RANGE` | no | not tested | no | not tested |
| `REDUCE_ANY` | no | not tested | yes | not tested |
| `REDUCE_MAX` | yes | works | yes | works |
| `REDUCE_MIN` | yes | works | yes | works |
| `REDUCE_PROD` | no | refused | yes | works |
| `RELU` | yes | works | yes | works |
| `RELU6` | yes | not tested | yes | not tested |
| `RELU_N1_TO_1` | yes | not tested | yes | not tested |
| `RESHAPE` | yes | works | yes | works |
| `RESIZE_BILINEAR` | yes | works | yes | works |
| `RESIZE_NEAREST_NEIGHBOR` | yes | works | yes | wrong result |
| `REVERSE_V2` | no | refused | no | CPU only |
| `ROUND` | no | works | no | CPU only |
| `RSQRT` | yes | works | yes | works |
| `SELECT` | yes | works | yes | not tested |
| `SELECT_V2` | no | not tested | yes | not tested |
| `SHAPE` | no | not tested | no | not tested |
| `SIN` | no | works | yes | works |
| `SLICE` | no | works | yes | CPU only |
| `SOFTMAX` | yes | works | yes | works |
| `SPACE_TO_BATCH_ND` | yes | works | yes | works |
| `SPACE_TO_DEPTH` | yes | works | yes | works |
| `SPLIT` | yes | works | yes | works |
| `SPLIT_V` | yes | refused | no | CPU only |
| `SQRT` | yes | works | yes | works |
| `SQUARE` | no | works | yes | works |
| `SQUARED_DIFFERENCE` | no | refused | no | CPU only |
| `SQUEEZE` | yes | not tested | yes | not tested |
| `STRIDED_SLICE` | yes | works | yes | works |
| `SUB` | yes | works | yes | works |
| `SUM` | yes | works | no | CPU only |
| `SVDF` | yes | not tested | no | not tested |
| `TANH` | yes | works | yes | works |
| `TILE` | yes | works | no | CPU only |
| `TOPK_V2` | no | refused | no | CPU only |
| `TRANSPOSE` | yes | works | yes | works |
| `TRANSPOSE_CONV` | yes | works | yes | crash |
| `UNIDIRECTIONAL_SEQUENCE_LSTM` | yes | not tested | no | not tested |
| `UNPACK` | yes | works | no | CPU only |
| `ZEROS_LIKE` | no | not tested | no | not tested |

Scripts, models and raw results (private notes): `rnd/verisilicon/models/gen_op_models.py`, `rknn_check.py`, `test_vx_ops.py`, `ops/*.tsv`.
