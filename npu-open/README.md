# npu-open: running neural networks on the NPU of the RK3399Pro without RKNN (experimental, milestone 1)

**Status: a working prototype, not part of the 0.3 image.** Tested on one board (2026-10-26). The release 0.3 still uses RKNN (see [docs/NPU.md](../docs/NPU.md)); this is the
replacement that the roadmap plans ([docs/ROADMAP.md](../docs/ROADMAP.md), "an open NPU stack on VeriSilicon's TIM-VX").

## What it is

The NPU die of the RK3399Pro is VeriSilicon (Vivante) IP. Rockchip's firmware for it contains Vivante's driver and libraries (SDK **6.4.6**). VeriSilicon publishes open (MIT) software
on top of them: **TIM-VX** and the **TFLite VX delegate**. The tag `v1.1.32` of TIM-VX ships the same SDK version as the firmware, so it runs on it unchanged. This directory adds what is
needed to use the NPU from the host:

```
host (the board, Debian, Python 3.13)                         NPU die (2 x A35, Rockchip's Linux 4.4 in RAM)
  NpuModel (host/npu_client.py)  <-- our USB bulk channel -->   npu_server (server/)  --> TIM-VX / TFLite VX delegate --> NPU
  models: .tflite, kept on the host                             one model at a time
```

* **Protocol** (`server/protocol.h`): `HELLO` (chip, driver, memory), `LOAD` (a `.tflite`), `LOAD_NBG` (a compiled graph), `RUN`, `UNLOAD`, `PING`; replies carry the tag of the request.
* **Class** `NpuModel(path_or_bytes)`: the constructor loads and prepares the model (and refuses to run on a chip that is not `0x8000 / 0x7100 / 0x45080001`, the only one of the Tinker Edge R),
  `exec(x)` runs synchronously, `async_exec(x, callback)` runs asynchronously (experimental, see below), `close()` / `del` unloads the model: the NPU is free again.
  One model per NPU; a second one raises an error. Float inputs are quantised with the scale and zero point of the model.
* **Cache of the compiled graph:** the first load of a model takes 13 s (the Vivante compiler runs on the two weak A35 cores of the NPU). The server returns the compiled graph (NBG), the host keeps it in
  `~/.cache/npu-models` (environment `NPU_CACHE`), and later loads take **0.1 s** and need no TFLite on the NPU.
* **Own USB channel** instead of ADB: the gadget of the NPU die has a FunctionFS function `ntb` (the one `rknn_server` uses); our server takes it over. Measured: 38 MB/s each way for a 150 KB frame
  (ADB: 5-6 MB/s), 0.3-0.5 ms for small messages.
* Needs **no Rockchip library above the driver** and no RKNN-Toolkit: models are plain TFLite (made with TensorFlow, any converter).

## Results (MobileNet V2 224, uint8, on the Tinker Edge R)

| | |
|---|---|
| load, first time / from the cache | 13 s / 0.1 s (memory on the NPU 101 MB / 6-17 MB) |
| `exec`, one request at a time | **9.1 ms per frame, 110 FPS** (the NPU itself 6.4 ms, the rest is transfer and copies) |
| `async_exec`, two in flight | **6.3 ms per frame, 158 FPS** |
| accuracy | the same top-1 class as TFLite on the CPU; cosine similarity 0.98 to the CPU result (uint8) |
| other numbers | float32 on the same NPU 478 ms (the fast path is integer); 104 of 104 unit tests of TIM-VX pass; RKNN: 89 inferences/s through its proxy |

## Known problems

* **`async_exec` can hang the USB bus.** A request sent immediately after a reply (as pipelining does) made the OUT transfer stall in about every other run; after the 60 s time-out the cancel (`stop endpoint`)
  killed the xHCI of the board (`HC died`, the whole USB 3 hub gone, the NPU stuck until it is reloaded). A pause of 300 us before each request (`async_gap_us`, the default) avoided it in all later tests
  (thousands of frames, several runs); `exec` has never hung. The real cause is not found (not libusb, not the NPU clock, not queue depth, not U1/U2). Recovery after a hang:
  `echo fe900000.usb > /sys/bus/platform/drivers/dwc3/unbind`, then `bind`, then reload the NPU (`systemctl restart npu-firmware`). The long list of what was tried is in the maintainer's notes (not in this repository).
* One model at a time; float32 models are slow (integer models only for speed); TFLite 2.6.0 on the NPU reads models of TensorFlow up to about that age: newer operator versions are untested.
* The build script was assembled from steps that were run by hand with the Ubuntu 20.04 toolchain image; it has not been run as a whole yet with the Debian 11 one.
* Interface numbers and endpoints are read from the USB descriptors (class 0xff / 0x4e / 0x50); the host needs access to `/dev/bus/usb` (root or a udev rule) and `npu-transfer-proxy` stopped.

## Layout

| Path | What |
|---|---|
| `server/` | the server on the NPU die (C++): `protocol.h`, `npu_server.cc`, `CMakeLists.txt` (built inside the delegate tree) |
| `host/` | `npu_client.py` (class `NpuModel`, `usbfs`, numpy only), `test_client.py`, `test_modes.py`, `stress_ping.py` |
| `ramdisk/modify_init.py` | changes the two init scripts of Rockchip's ramdisk so that our server starts instead of `rknn_server` (with a fallback to it) |
| `tools/npu_ramdisk.py` | list, extract, add, remove files in the ramdisk of a `boot.img` without root |
| `patches/` | the changes to `tflite-vx-delegate` (build fixes for a current CMake; `VX_NBG_EXPORT` writes the compiled graph) |
| `build/` | `Dockerfile` of the cross toolchain (Debian 11: glibc 2.31 headers, nothing newer references `GLIBC_2.34`; Alpine / musl cannot load the glibc libraries of the driver) and `build.sh` |

## Licences

Our code: MIT. TIM-VX and `tflite-vx-delegate`: MIT (VeriSilicon); TensorFlow Lite: Apache-2.0. The Vivante libraries and `galcore` come from Rockchip's firmware (closed binaries) and are not stored here.
