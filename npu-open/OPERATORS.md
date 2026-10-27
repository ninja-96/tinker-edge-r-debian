# TensorFlow Lite operators on the NPU: release 0.4.1, release 0.4 and RKNN-Toolkit 1.7.5

**Release 0.4.1** (TensorFlow Lite 2.21, TIM-VX and tflite-vx-delegate of their `main` branches, measured on 2026-10-27): of the 87 single-operator models **68 run entirely on
the NPU, all with the right result**; 19 have an operator that the NPU does not take (it runs on the CPU of the NPU die with `device="auto"`; `device="npu"` refuses such a model
and names the operator). No model gives a wrong result or crashes. Compared with 0.4, these now run on the NPU and right: `FLOOR`, `FLOOR_DIV`, float `FULLY_CONNECTED`, `GATHER_ND`
(was wrong), `MIRROR_PAD`, `ONE_HOT`, `RESIZE_NEAREST_NEIGHBOR` (was wrong), `REVERSE_V2`, `SPLIT_V`, `SQUARED_DIFFERENCE`, `SUM`, float `TRANSPOSE_CONV` (crashed), `UNPACK`.

| Release 0.4.1 | Operators |
|---|---|
| on the NPU, right result | 68 of 87 models (all the operators marked **works** below) |
| on the CPU of the NPU die | `CEIL`, `COS`, `ROUND`, `LOG_SOFTMAX`, `TILE`, `TOPK_V2`, `SLICE` (in the `BATCH_MATMUL` model); comparisons (`EQUAL`, `NOT_EQUAL`, `LESS`, `LESS_EQUAL`, `GREATER`, `GREATER_EQUAL`), logical operators (`LOGICAL_AND`, `LOGICAL_OR`, `LOGICAL_NOT`) and `CAST`: the new delegate takes them, but TIM-VX crashes on bool tensors with this driver, so our patch keeps them on the CPU |
| wrong or crashing | none |

Section "Release 0.4" below is the measurement of 2026-10-26 (the 0.4 stack against RKNN-Toolkit 1.7.5), kept for the comparison.

## Release 0.4 (2026-10-26)

Measured on one Tinker Edge R (RK3399Pro, Vivante driver 6.4.6). The question then: **which operators of a TFLite model work with the open NPU stack and which with RKNN**,
so that the RKNN path can be dropped when the open stack has parity.

### Method

* The documented lists: RKNN from `RKNN_OP_Support_V1.7.5.md` (Rockchip), the open stack from the operator table in `op_map.cc` of the delegate (tag `tflite_v2.6.0`).
  The documents are not the whole truth, so every operator was also tested.
* For each operator one tiny float32 TFLite model (input 1x8x8x4, TensorFlow 2.2: the schema RKNN-Toolkit 1.7.5 reads), 87 models in all.
  The converter sometimes rewrites an operator (`DIV` became `MUL`, `RELU6` became `MUL`, `SQUEEZE` and `EXPAND_DIMS` became `RESHAPE`) or adds a helper (`CAST` after a comparison):
  such an operator counts as **not tested**, and a failure is blamed on an operator only when it is the only suspect.
* **RKNN:** `load_tflite()` and `build(do_quantization=False)` of the Toolkit. The PC simulator no longer exists after Toolkit 1.7.3, so *numbers were not checked*:
  "works" means that the Toolkit accepts the model; it does not say whether the operator then runs on the NPU or on a CPU.
* **Open stack:** the model is loaded by the server with `NPU_STRICT=1` (0.4.1: `device="npu"`, the same check): the load is refused if the delegate left any operator to the CPU, and the
  names of those operators are returned. So "works" means *the operator runs on the NPU*, and the result was compared with TensorFlow on the CPU (tolerance 0.05). The 0.4.1 column uses
  the same 87 models and the same input.
* Float32 models were used for coverage. Per-operator behaviour of the int8 / uint8 paths is not covered here (the whole-network results are in `README.md`).

### Result

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

### Per operator

`RKNN list` = named in the Rockchip document; `RKNN tested` = accepted by the Toolkit 1.7.5; `VX code (0.4)` = has a mapper in `op_map.cc` of the tag `tflite_v2.6.0`; the last two columns = what the board did with the open stack of 0.4 and of 0.4.1 ("CPU": the operator stays on the CPU of the NPU die; in brackets: another operator of the same model that does).

| TFLite operator | RKNN list | RKNN tested | VX code (0.4) | 0.4 (TIM-VX v1.1.32, TFLite 2.6) | **0.4.1** (TIM-VX / delegate `main`, TFLite 2.21) |
|---|---|---|---|---|---|
| `ABS` | no | works | yes | works | **works** |
| `ADD` | yes | works | yes | works | **works** |
| `ADD_N` | no | works | yes | works | **works** |
| `ARG_MAX` | yes | works | yes | works | **works** |
| `ARG_MIN` | yes | works | yes | works | **works** |
| `AVERAGE_POOL_2D` | yes | works | yes | works | **works** |
| `BATCH_MATMUL` | no | not tested | no | not tested | CPU (`SLICE`) |
| `BATCH_TO_SPACE_ND` | yes | works | yes | not tested | CPU (`TILE`) |
| `CAST` | no | refused | no | CPU only | CPU |
| `CEIL` | no | works | no | CPU only | CPU |
| `CONCATENATION` | yes | works | yes | works | **works** |
| `CONV_2D` | yes | works | yes | works | **works** |
| `COS` | no | works | no | CPU only | CPU |
| `DEPTHWISE_CONV_2D` | yes | works | yes | works | **works** |
| `DEPTH_TO_SPACE` | yes | works | yes | works | **works** |
| `DEQUANTIZE` | yes | not tested | yes | not tested | not tested |
| `DIV` | yes | not tested | yes | not tested | **works** |
| `ELU` | no | works | yes | works | **works** |
| `EQUAL` | no | refused (with CAST) | no | CPU only | CPU (`CAST`) |
| `EXP` | no | works | yes | works | **works** |
| `EXPAND_DIMS` | no | not tested | yes | not tested | **works** |
| `FILL` | no | not tested | no | not tested | **works** |
| `FLOOR` | yes | works | no | CPU only | **works** |
| `FLOOR_DIV` | yes | works | no | CPU only | **works** |
| `FULLY_CONNECTED` | yes | works | yes | crash | **works** |
| `GATHER` | yes | works | yes | works | **works** |
| `GATHER_ND` | no | works | yes | wrong result | **works** |
| `GREATER` | yes | works | no | CPU only | CPU (`CAST`) |
| `GREATER_EQUAL` | yes | refused (with CAST) | no | CPU only | CPU (`CAST`) |
| `HARD_SWISH` | no | works | yes | works | **works** |
| `L2_NORMALIZATION` | yes | refused | yes | works | **works** |
| `L2_POOL_2D` | yes | not tested | no | not tested | not tested |
| `LEAKY_RELU` | yes | works | yes | works | **works** |
| `LESS` | yes | refused (with CAST) | no | CPU only | CPU (`CAST`) |
| `LESS_EQUAL` | yes | refused (with CAST) | no | CPU only | CPU (`CAST`) |
| `LOCAL_RESPONSE_NORMALIZATION` | yes | works | yes | works | **works** |
| `LOG` | no | works | yes | works | **works** |
| `LOGICAL_AND` | no | refused (with CAST) | yes | not tested | CPU (`GREATER`, `CAST`) |
| `LOGICAL_NOT` | no | not tested | yes | not tested | CPU (`LESS_EQUAL`, `CAST`) |
| `LOGICAL_OR` | no | refused (with CAST) | yes | not tested | CPU (`GREATER`, `CAST`) |
| `LOGISTIC` | yes | works | yes | works | **works** |
| `LOG_SOFTMAX` | yes | works | no | CPU only | CPU |
| `LSTM` | yes | not tested | no | not tested | not tested |
| `MAXIMUM` | no | works | yes | works | **works** |
| `MAX_POOL_2D` | yes | works | yes | works | **works** |
| `MEAN` | no | works | yes | works | **works** |
| `MINIMUM` | no | works | yes | works | **works** |
| `MIRROR_PAD` | yes | works | no | CPU only | **works** |
| `MUL` | yes | works | yes | works | **works** |
| `NEG` | yes | works | yes | works | **works** |
| `NOT_EQUAL` | yes | refused (with CAST) | no | CPU only | CPU (`CAST`) |
| `ONE_HOT` | no | refused | no | CPU only | **works** |
| `PACK` | no | works | yes | works | **works** |
| `PAD` | yes | works | yes | works | **works** |
| `POW` | yes | works | yes | works | **works** |
| `PRELU` | yes | not tested | yes | not tested | not tested |
| `QUANTIZE` | no | not tested | yes | not tested | not tested |
| `RANGE` | no | not tested | no | not tested | **works** |
| `REDUCE_ANY` | no | not tested | yes | not tested | not tested |
| `REDUCE_MAX` | yes | works | yes | works | **works** |
| `REDUCE_MIN` | yes | works | yes | works | **works** |
| `REDUCE_PROD` | no | refused | yes | works | **works** |
| `RELU` | yes | works | yes | works | **works** |
| `RELU6` | yes | not tested | yes | not tested | **works** |
| `RELU_N1_TO_1` | yes | not tested | yes | not tested | not tested |
| `RESHAPE` | yes | works | yes | works | **works** |
| `RESIZE_BILINEAR` | yes | works | yes | works | **works** |
| `RESIZE_NEAREST_NEIGHBOR` | yes | works | yes | wrong result | **works** |
| `REVERSE_V2` | no | refused | no | CPU only | **works** |
| `ROUND` | no | works | no | CPU only | CPU |
| `RSQRT` | yes | works | yes | works | **works** |
| `SELECT` | yes | works | yes | not tested | CPU (`GREATER`) |
| `SELECT_V2` | no | not tested | yes | not tested | not tested |
| `SHAPE` | no | not tested | no | not tested | **works** |
| `SIN` | no | works | yes | works | **works** |
| `SLICE` | no | works | yes | CPU only | **works** |
| `SOFTMAX` | yes | works | yes | works | **works** |
| `SPACE_TO_BATCH_ND` | yes | works | yes | works | **works** |
| `SPACE_TO_DEPTH` | yes | works | yes | works | **works** |
| `SPLIT` | yes | works | yes | works | **works** |
| `SPLIT_V` | yes | refused | no | CPU only | **works** |
| `SQRT` | yes | works | yes | works | **works** |
| `SQUARE` | no | works | yes | works | **works** |
| `SQUARED_DIFFERENCE` | no | refused | no | CPU only | **works** |
| `SQUEEZE` | yes | not tested | yes | not tested | **works** |
| `STRIDED_SLICE` | yes | works | yes | works | **works** |
| `SUB` | yes | works | yes | works | **works** |
| `SUM` | yes | works | no | CPU only | **works** |
| `SVDF` | yes | not tested | no | not tested | not tested |
| `TANH` | yes | works | yes | works | **works** |
| `TILE` | yes | works | no | CPU only | CPU |
| `TOPK_V2` | no | refused | no | CPU only | CPU |
| `TRANSPOSE` | yes | works | yes | works | **works** |
| `TRANSPOSE_CONV` | yes | works | yes | crash | **works** |
| `UNIDIRECTIONAL_SEQUENCE_LSTM` | yes | not tested | no | not tested | not tested |
| `UNPACK` | yes | works | no | CPU only | **works** |
| `ZEROS_LIKE` | no | not tested | no | not tested | **works** |

Scripts, models and raw results (private notes): `rnd/verisilicon/models/gen_op_models.py`, `rknn_check.py`, `test_vx_ops.py` (0.4), `test_ops_041.py` (0.4.1), `ops/*.tsv`.
