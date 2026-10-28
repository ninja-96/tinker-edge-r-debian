# npu-open: the open NPU stack

Since release 0.4 this is how the NPU of the RK3399Pro is used: TensorFlow Lite models, no RKNN. **How to use it is in [docs/NPU.md](../docs/NPU.md)**; this directory holds
what runs on the NPU die, the board side (daemon, C++ library, Python binding) and how it is built. Measured operator coverage: [OPERATORS.md](OPERATORS.md).

The NPU die of the RK3399Pro is VeriSilicon (Vivante) IP. Rockchip's firmware for it contains Vivante's driver and libraries (SDK **6.4.6**). VeriSilicon publishes open (MIT) software
on top of them: **TIM-VX** and the **TFLite VX delegate**. Release 0.4.1 uses their `main` branches with TensorFlow Lite **2.21.0**. TIM-VX is built with the SDK headers of its tag
v1.1.32 (6.4.6, the driver's version): the headers of newer SDKs enlarge `vx_hardware_caps_params_ext_t`, which the 6.4.6 driver refuses with -10 (that, not the driver, made
TIM-VX v1.1.57 crash in 0.4); other `VX_*_SUPPORT` macros of the headers switch off what the driver cannot do. All TIM-VX unit tests that exist in v1.1.32 pass; the failing new ones
are operators newer than the driver (Conv3d, reflect padding, RoiPool, HashtableLookup).

```
board (Debian 13, Python 3.13)                                                     NPU die (2 x A35, Rockchip's Linux 4.4 in RAM)
 programs: rk3399pro_npu (Python) / librk3399pro_npu (C++)                          npu_server (server/): one child "engine" per model:
   --unix socket /run/npu/npu.sock--> npu-daemon --USB bulk channel ("ntb")-->        TFLite 2.21 + XNNPACK + VX delegate -> TIM-VX -> Vivante driver
 models: .tflite, kept on the board; compiled graphs in ~/.cache/npu-models          several models at once, loaded until stopped
```

* **Protocol 2** (`server/protocol.h`): 20-byte header `{magic "NPU2", type, flags, tag, model, length}`; `HELLO` (chip, driver, memory, number of models), `LOAD` (a `.tflite`
  with its key - the sha256 of the file -, a name and a plan), `LOAD_NBG` (a compiled graph), `RUN`, `UNLOAD` (one model or all), `LIST`, `DESCRIBE` (the tensors of a
  loaded model, to attach to it), `OPS` (the operators of a model and where they run), `PING`; replies carry the tag and the model of the request. A message never ends on a
  multiple of 1024 bytes (flag `PAD`): the USB controller would wait for a short packet.
* **Server** (`server/npu_server.cc`): the parent process owns the USB function, the protocol and the table of models; every model lives in a child process
  (`npu_server --engine`), so a segmentation fault or a hang of the delegate or the driver becomes an error reply with the signal and the last lines of its log
  (`/tmp/npu_engine.<id>.log` on the NPU die), and the other models and the parent stay. A `LOAD` with a key that is loaded already returns that model (attach). A `LOAD` runs in a
  thread of its own (one at a time), so the loaded models keep running. Models stay loaded when the host goes away. It checks the chip identity
  (`/sys/devices/platform/ffbc0000.npu/info`: model 0x8000, revision 0x7100, product 0x45080001) and the free memory. The plan of a `LOAD`: CPU only (no delegate), no split of
  per-channel layers, operator types left to the CPU (the delegate skips them: `VX_DENY_OPS`). `NPU_STRICT=1` (test mode) refuses a model that has an operator on the CPU and names it.
* **Model rewrites before the graph is built**: Keras tails `SHAPE -> STRIDED_SLICE -> PACK -> RESHAPE` get a constant shape (`server/fix_dynamic_reshape.h`); per-channel int8 layers
  are split into per-tensor groups (`server/split_per_channel.h`, see "Why integer models" in [docs/NPU.md](../docs/NPU.md)). int8 tensors reach the NPU as uint8 (the NPU is wrong for signed int8).
* **Compiled graph cache:** the delegate writes the compiled graph (NBG) to `VX_NBG_EXPORT=<file>`; the server returns it with the `LOAD` reply only when the whole model became one NPU
  graph, the library keeps it per plan; `LOAD_NBG` then needs no TFLite on the NPU (0.04-0.3 s). Models with a float input or output are not cached (their graph uses float16 inside).
* **Board side** (`host/`): `npu_daemon.cc` holds the USB channel (usbfs) and passes the messages of any number of programs (it replaces their tags with its own); `rk3399pro_npu.hpp` /
  `rk3399pro_npu.cc` is the C++ library (connection to the daemon or straight to USB, models, attach, memory check, compiled-graph cache, `device` plans, `autotune`, sha256 in
  `sha256.h`, operator names in `op_names.h`, generated from the TFLite 2.21 schema); `python_core.cc` is its Python binding (`rk3399pro_npu._core`, pybind11); `python/rk3399pro_npu`
  is the Python package around it.
* **USB rules found:** write the FunctionFS descriptors **before** the USB controller is bound (our `S50usbdevice` in the NPU ramdisk; the gadget of this firmware is unbound and bound
  at every disconnect, so the server reads the ep0 events and writes the descriptors again after UNBIND); OUT reads are multiples of 1024 bytes (AIO, four armed 64 KiB reads); the
  interface class is 0xff/0x4e/0x50 so that ADB does not take it. The board side uses usbfs (`USBDEVFS_BULK`, one blocking call per thread).
* **The delegate** needs our patch (`patches/`): `VX_NBG_EXPORT`, int8 handed to the NPU as uint8, constant `Dequantize` (float16 weights) folded on the CPU, the buffers it builds kept
  alive (TIM-VX keeps the data pointer of constants and reads it again in layout inference), `VX_DENY_OPS`, the server as a build target. `VX_REQUANT_PER_CHANNEL=1` (one scale per
  layer, fast, **loses accuracy**) and `VX_FP16=1` (all-float16 graph, crashes TIM-VX) are experiments, off.

## Known problems

* **`async_exec` can hang the USB bus** (see docs/NPU.md); `exec` never hung.
* Operators: [OPERATORS.md](OPERATORS.md): 68 of 87 single-operator models run entirely on the NPU with the right result, the others keep an operator on the CPU; none is wrong.
* A TIM-VX unit test of `main` crashes (`Stack` with int32); a model that hits it crashes only its own engine.

## Layout

| Path | What |
|---|---|
| `server/` | the server for the NPU die (C++): `protocol.h`, `npu_server.cc`, `fix_dynamic_reshape.h`, `split_per_channel.h`, `CMakeLists.txt` (built inside the delegate tree) |
| `host/` | the board side (C++): `npu_daemon.cc`, `rk3399pro_npu.hpp`, `rk3399pro_npu.cc`, `python_core.cc`, `sha256.h`, `op_names.h` |
| `patches/` | the changes to `tflite-vx-delegate` (`0001`, `0003`) and to TensorFlow Lite (`0002`) |
| `ramdisk/modify_init.py` | changes the two init scripts of Rockchip's ramdisk so that our server starts instead of `rknn_server` (with a fallback to it) |
| `tools/npu_ramdisk.py` | list, extract, add, remove files in the ramdisk of a `boot.img` without root (pure Python cpio) |
| `OPERATORS.md` | the operators on the NPU, measured: 0.4.1, 0.4 and RKNN-Toolkit 1.7.5 |

Build: `scripts/build-npu-open.sh` (Docker, Debian 11 toolchain from `docker/npu-xtool`, sources pinned in `sources.lock`), then `./run.sh scripts/build-npu-boot.sh`; the board side:
`./run.sh scripts/build-npu-host.sh`. Python package: `python/rk3399pro_npu`. Tests: `tests/npu/` (the self-test models) and `tests/npu/dev/` (mode tests used while developing).

## Licences

Our code: MIT. TIM-VX and `tflite-vx-delegate`: MIT (VeriSilicon); TensorFlow Lite: Apache-2.0; what TensorFlow Lite links in (XNNPACK, cpuinfo, pthreadpool: BSD; abseil, flatbuffers,
gemmlowp, ruy, ml_dtypes: Apache-2.0; Eigen: MPL-2.0; FP16, FXdiv, farmhash: MIT; fft2d: Ooura's permissive licence) with their texts in the image; pybind11 (BSD-3-Clause, headers
from Debian). The Vivante libraries and `galcore` come from Rockchip's firmware (closed binaries) and are not stored here.
