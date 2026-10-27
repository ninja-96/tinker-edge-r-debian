# npu-open: the open NPU stack (sources for the NPU die)

Since release 0.4 this is how the NPU of the RK3399Pro is used: TensorFlow Lite models, no RKNN. **How to use it is in [docs/NPU.md](../docs/NPU.md)**; this directory holds what runs on the NPU
die and how it is built. Measured operator coverage against RKNN-Toolkit 1.7.5: [OPERATORS.md](OPERATORS.md).

The NPU die of the RK3399Pro is VeriSilicon (Vivante) IP. Rockchip's firmware for it contains Vivante's driver and libraries (SDK **6.4.6**). VeriSilicon publishes open (MIT) software
on top of them: **TIM-VX** and the **TFLite VX delegate**. The tag `v1.1.32` of TIM-VX ships the same SDK version as the firmware, so it runs on it unchanged (v1.1.57 crashes on this driver).

```
board (Debian 13, Python 3.13)                                   NPU die (2 x A35, Rockchip's Linux 4.4 in RAM)
  python/rk3399pro_npu  <-- our USB bulk channel ("ntb") -->     npu_server (server/) -> child "engine": TFLite 2.6 + VX delegate -> TIM-VX -> Vivante driver
  models: .tflite, kept on the board                             one model at a time
```

* **Protocol** (`server/protocol.h`): 16-byte header `{magic "NPU1", type, flags, tag, length}`; `HELLO` (chip, driver, memory), `LOAD` (a `.tflite`), `LOAD_NBG` (a compiled graph), `RUN`, `UNLOAD`, `PING`;
  replies carry the tag of the request. A message never ends on a multiple of 1024 bytes (flag `PAD`): the USB controller would wait for a short packet.
* **Server** (`server/npu_server.cc`): the parent process owns the USB function and the protocol; the model lives in a child process (`npu_server --engine`), so a segmentation fault or a hang of the delegate or the
  driver is turned into an error reply with the signal and the last lines of the log (`/tmp/npu_engine.log` on the NPU die), and the parent stays on the bus. It checks the chip identity
  (`/sys/devices/platform/ffbc0000.npu/info`: model 0x8000, revision 0x7100, product 0x45080001) and the free memory. `NPU_STRICT=1` (test mode) refuses a model that has an operator on the CPU and names it; `NPU_SPLIT=0` switches off the split of per-channel layers.
* **Per-channel int8** (`server/split_per_channel.h`): see "Why integer models" in [docs/NPU.md](../docs/NPU.md). int8 tensors reach the NPU as uint8 (the NPU is wrong for signed int8).
* **Compiled graph cache:** the delegate writes the compiled graph (NBG) to `VX_NBG_EXPORT=<file>`; the server returns it with the `LOAD` reply and the host keeps it (`~/.cache/npu-models`); `LOAD_NBG` then needs no TFLite on the NPU (0.1-0.3 s).
  Models with a float input or output are not cached (the graph of those uses float16 inside).
* **USB rules found:** write the FunctionFS descriptors **before** the USB controller is bound (our `S50usbdevice` in the NPU ramdisk; the gadget of this firmware is unbound and bound at every disconnect, so the server reads the ep0
  events and writes the descriptors again after UNBIND); OUT reads are multiples of 1024 bytes (AIO, four armed 64 KiB reads); the interface class is 0xff/0x4e/0x50 so that ADB does not take it.
  The host side uses usbfs (`USBDEVFS_BULK` through ctypes, one blocking call per thread).
* **The delegate** needs our patch (`patches/`): build fixes for a current CMake, `VX_NBG_EXPORT`, int8 handed to the NPU as uint8, constant `Dequantize` (float16 weights) folded on the CPU, the buffers it builds kept alive
  (TIM-VX keeps the data pointer of constants and reads it again in layout inference). `VX_REQUANT_PER_CHANNEL=1` (one scale per layer, fast, **loses accuracy**) and `VX_FP16=1` (all-float16 graph, crashes TIM-VX) are experiments, off.

## Known problems

* **`async_exec` can hang the USB bus** (see docs/NPU.md); `exec` never hung.
* Operators: [OPERATORS.md](OPERATORS.md). Keras models with a dynamic `Reshape` tail are refused by the delegate.

## Layout

| Path | What |
|---|---|
| `server/` | the server for the NPU die (C++): `protocol.h`, `npu_server.cc`, `split_per_channel.h`, `CMakeLists.txt` (built inside the delegate tree) |
| `patches/` | the changes to `tflite-vx-delegate` |
| `ramdisk/modify_init.py` | changes the two init scripts of Rockchip's ramdisk so that our server starts instead of `rknn_server` (with a fallback to it) |
| `tools/npu_ramdisk.py` | list, extract, add, remove files in the ramdisk of a `boot.img` without root (pure Python cpio) |
| `OPERATORS.md` | the operators, RKNN-Toolkit 1.7.5 against this stack, measured |

Build: `scripts/build-npu-open.sh` (Docker, Debian 11 toolchain from `docker/npu-xtool`, sources pinned in `sources.lock`), then `./run.sh scripts/build-npu-boot.sh`. Python module: `python/rk3399pro_npu`. Tests: `tests/npu/` (the self-test models) and
`tests/npu/dev/` (stress and mode tests used while developing).

## Licences

Our code: MIT. TIM-VX and `tflite-vx-delegate`: MIT (VeriSilicon); TensorFlow Lite: Apache-2.0. The Vivante libraries and `galcore` come from Rockchip's firmware (closed binaries) and are not stored here.
