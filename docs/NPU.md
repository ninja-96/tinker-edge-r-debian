# RK3399Pro NPU on the Tinker Edge R: TensorFlow Lite

> **Status (release 0.4.1): TensorFlow Lite 2.21 models run on the NPU through an open software stack**, several at once and from several programs.
> The NPU die runs Linux 4.4.185 and the Vivante driver from Rockchip's firmware (closed, Apache-2.0 repository `airockchip/RK3399Pro_npu`, used as it is except for
> one device tree change). Everything above the driver is open: [TIM-VX](https://github.com/VeriSilicon/TIM-VX), VeriSilicon's TFLite VX delegate, TensorFlow Lite 2.21
> with XNNPACK, our server on the NPU die, our USB channel, the daemon `npu-daemon`, the C++ library `librk3399pro_npu` and its Python module `rk3399pro_npu`.
> The numbers below were measured on a board; the research behind them is in [NPU-RESEARCH.md](NPU-RESEARCH.md) and `npu-open/`.

## How it works

```
 board (RK3399Pro host, Debian 13)                                  NPU die (2 x Cortex-A35, 2 GB, Linux 4.4.185 from RAM)
 program A: rk3399pro_npu.Model("a.tflite") --+                        npu_server (our): one child process ("engine") per model:
 program B: rk3399pro_npu.Model("b.tflite") --+-- npu-daemon -- USB 3 -->  TensorFlow Lite 2.21 + VX delegate (VeriSilicon) -> TIM-VX -> libVSC/galcore
 (C++: librk3399pro_npu, the same)              /run/npu/npu.sock          + XNNPACK on the A35 for what stays on the CPU
                                                own bulk channel ("ntb"), 38-52 MB/s
```

* The model file stays on the board. A model is identified by the **sha256 of its file**. `Model(path)` looks at the NPU first: if the model is loaded (by this or
  another program) it **attaches** to it (a few milliseconds, nothing is loaded). Otherwise it checks that the model **fits into the free memory** of the NPU die
  (the memory measured at an earlier load of the same model, or an estimate) and loads it: the server builds the graph for the NPU (**the first time takes seconds
  to minutes**), the compiled graph is cached on the board (`~/.cache/npu-models`) and the next load takes **0.04-0.3 s**.
* **Several models** can be loaded at once, each in its own child process ("engine") of the server: a model that crashes the delegate or the driver is answered with
  the reason (`NpuError`) and the other models, the NPU and the USB link stay up. A long load does not stop the loaded models.
* A model **stays loaded until it is stopped**: `close()` only lets go of it (other programs may use it), `stop()` (or `npu.stop(id or name)`, `npu-models stop`) unloads it.
* **Several programs** use the NPU through the daemon `npu-daemon` (systemd, socket `/run/npu/npu.sock`, group `plugdev`). Without it (or with `NPU_DIRECT=1`) one program
  talks to the NPU over USB itself. The runs of one model are served one after another; the runs of different models interleave.
* **Where a model runs** (its *plan*), `Model(path, device=...)`:
  * `"auto"` (default): NPU and CPU together. The plan `autotune()` found for this model if there is one; else integer models (int8, uint8) on the NPU with what the NPU
    cannot do on the CPU of the NPU die, and float models on that CPU (TensorFlow Lite with XNNPACK on its two Cortex-A35): this NPU runs float16 on 2 of its 10 cores
    and is 2-2.5 times slower than that CPU for float models, and gave wrong results for some of them.
  * `"npu"`: strictly the whole model on the NPU. If an operator would stay on the CPU, the load fails with `NpuError` naming it and recommending `"auto"`.
  * `"cpu"`: strictly the whole model on the CPU of the NPU die.
  Comparisons, logical operators and `CAST` (bool tensors) are never given to the NPU: they crash TIM-VX on this driver. `Model.autotune()` measures the plans of one model (whole NPU, NPU without the per-channel split, mixed: operator types moved to the CPU, CPU only)
  against the CPU result and keeps the fastest one with the same result (cosine similarity of the outputs at least 0.99); later loads use it.
* The server checks the chip identity (model 0x8000, revision 0x7100, product 0x45080001 - every Tinker Edge R has it, see "The chip" below) and refuses to run on anything else.

## The chip

Read from the NPU die (sysfs of `galcore`) and from the feature table inside Rockchip's driver (6.4.6.5), decoded with the layout of the open galcore 6.4.6.2 of Amlogic/Khadas:

| | |
|---|---|
| IP | VeriSilicon (Vivante) VIP8000 family, the "VIPNano-Q" class; the closest relative is the NPU of the Amlogic A311D (`vipnano-qi`, revision 0x7120): their feature tables differ in 17 of 733 fields |
| Identity | model 0x8000, revision 0x7100, product 0x45080001, customer 0x82 (Rockchip); not in mainline etnaviv |
| Compute | 10 NN cores x 64 MAC with 3-way dot products = 1920 int8 MAC per cycle, about 3.0 TOPS at 792 MHz (the clock of the firmware); int16 and float16 on 2 of the 10 cores; 6 tensor processors; one small shader core |
| Memory | 512 KiB VIP SRAM, 1984 KiB AXI SRAM; the NPU die has its own 2 GiB of DRAM |
| Not in the hardware | per-channel quantisation (`NN_PER_CHANNEL_QUANT = 0`: why per-channel int8 is slow and the server splits such layers), native depthwise convolution, element-wise add in the NN engine, bfloat16 |
| Measured | signed int8 tensors give wrong numbers (the same values as uint8 are right: the delegate shifts them) |

## Transport: USB

The stock device tree has no `rockchip,deferred` property, so the vendor
kernel never creates `pcie_reset_ep`, and the ASUS `npu-image.sh` takes the
USB branch (`npu_upgrade` + `/usr/share/npu_fw`). No PCIe patch is needed; the
BSP's `rockchip-pcie-dma` and deferred-link code are not used on this board.
(The external PCIe root complex carries the Wi-Fi module.)

## Power sequence (reconstructed from `npu_powerctrl` V1.1)

Vendor GPIO numbers are `bank * 32 + pin`.

| Step | Action |
|---|---|
| 1 | PMIC `rk808-clkout2` (32 kHz) on, `clk_wifi_pmu` off |
| 2 | GPIOs 56, 55, 54, 11, 4, 10, 36, 32 low |
| 3 | GPIO 4, 10, 11 high, 2 ms apart (rails) |
| 4 | `clk_wifi_pmu` on (NPU reference clock, pin GPIO0_A2 function 1) |
| 5 | GPIO 54, 55, 56 high 2 ms apart, 25 ms wait, GPIO 32 high (release) |

GPIO 35 is an input (sleep state) and GPIO 36 is pulsed on suspend/resume;
suspend/resume handling is not implemented yet.

The vendor kernel flipped the clock through a debugfs file that mainline lacks,
so `kernel-mods/drivers/soc/rockchip/tinker-npu-power.c` does the sequence from
the device tree (`rk3399pro-tinker-edge-r-npu.dtsi`) and exposes
`/sys/bus/platform/drivers/tinker-npu-power/*/{enable,reset}`.

Two details that cost a day to find, both fixed in the driver and the device tree:

- The reference clock is `clk_wifi_pmu` of the **PMU** clock controller (`&pmucru`), and it
  must run at **24 MHz**: the NPU die takes it as its crystal input. The default of the
  clock tree is 26 MHz, with which the NPU's BootROM never shows up on USB. The driver sets
  24 MHz before enabling the clock.
- Once powered, the NPU appears on USB as `2207:180a` (Maskrom, port 4 of the on-board
  USB hub); after the firmware is loaded it re-enumerates as `2207:0019` (SuperSpeed) with an
  ADB and an `ntb` interface (the open channel of this software; Rockchip's own firmware uses it for RKNN).

## What is installed

| File | Origin |
|---|---|
| firmware `MiniLoaderAll.bin`, `uboot.img`, `trust.img`, `boot.img` in `/usr/share/npu_fw` | Rockchip, https://github.com/airockchip/RK3399Pro_npu at the commit in `sources.lock`, fetched and sha256-checked by `scripts/fetch-npu.sh`, binary only. **Closed**: Linux 4.4.185 with the `galcore` driver, the Vivante user libraries (SDK 6.4.6), BusyBox. `scripts/build-npu-boot.sh` changes two things in `boot.img`: the device tree (below) and the ramdisk (our files, below) |
| `npu_server`, `libvx_delegate.so`, `libtim-vx.so` (inside `boot.img`, run on the NPU die) | built from source by `scripts/build-npu-open.sh` (TIM-VX `main` with the SDK 6.4.6 headers of v1.1.32, tflite-vx-delegate `main` with [our patch](../npu-open/patches), TensorFlow Lite 2.21.0 with XNNPACK and its dependencies, our `npu-open/server/`; all pinned in `sources.lock`), `-O3 -mcpu=cortex-a35`, in a Debian 11 container |
| `npu-daemon` (`/usr/sbin`), `librk3399pro_npu.so.0`, `rk3399pro_npu.hpp`, the Python module's `_core` | built from `npu-open/host/` by `scripts/build-npu-host.sh` (C++17, pybind11 from Debian, `-O3 -mcpu=cortex-a72.cortex-a53`) |
| `rkdeveloptool` with the `rs` command | `third_party/rkdeveloptool` plus `patches/rkdeveloptool/` (GPL-2.0+), built for arm64 by `scripts/build-rkdeveloptool-arm64.sh` |
| `npu-load`, `npu-firmware.service`, `npu-daemon.service`, `60-tinker-npu.rules` | this repository, `overlay/` |
| `rk3399pro_npu` (Python module), `npu-info`, `npu-models`, `npu-selftest` | this repository, `python/` and `overlay/` (MIT) |

**Loading the firmware needs no closed tool.** The loader sequence of the stock `upgrade_tool`
(`db`, `td`, `wl` x3, `rs`) is done by `rkdeveloptool`; the missing `rs` ("run system") was added after
watching its USB traffic, see the patch.

**What is changed in Rockchip's firmware image.**

1. *Device tree.* The image is made for Rockchip's evaluation board, where the NPU is powered by two I2C regulators (`tcs452x`, `syr837`). On the Tinker Edge R the
   kernel of the image fails with `galcore: failed to get regulator` and the NPU resets the moment a model is loaded. The ASUS image has a `fan53555` there, which the
   Rockchip kernel's driver does not accept. `scripts/npu-fw-tinker.py` replaces the two regulators with one fixed regulator (0.85 V, no control over I2C, so the NPU stays at
   the voltage that the board sets by default).
2. *Ramdisk.* Our three files are added, and the two init scripts (`S50usbdevice`, `S60NPU_init`) are changed (`npu-open/ramdisk/modify_init.py`) so that `npu_server` takes the USB
   function `ntb` (it must write its descriptors before the USB controller is bound) instead of Rockchip's `rknn_server`. If the server is not ready in 10 s the stock `rknn_server` is
   started instead, so that a broken server cannot leave the NPU without a USB link (ADB still works).

Services (active only with an `-npu` or `-full` DTB, via `ConditionPathExists=/proc/device-tree/npu-power`): `npu-firmware.service` runs `npu-load` (reset, `rkdeveloptool db/td/wl/rs`).
Enable on the board: `sudo tinker-boot-select npu` (or `full`, the default), reboot.

Access: the NPU appears on USB as `2207:0019`; the udev rule `60-tinker-npu.rules` gives it to the group `plugdev`; the daemon (a dynamic system user in `plugdev`) holds it
and its socket `/run/npu/npu.sock` belongs to `plugdev` too, so the default user needs no root.

## Run your own model

1. **Make a `.tflite` file on the PC** with any converter of TensorFlow (or another tool that writes TFLite). Rules that matter:
   * **Use an integer model for the NPU** - full 8-bit quantisation (int8 or uint8). `tf.lite.Optimize.DEFAULT` with a representative dataset (a few dozen real inputs),
     `target_spec.supported_ops = [TFLITE_BUILTINS_INT8]`; float inputs and outputs (a `QUANTIZE` / `DEQUANTIZE` at the ends) are fine. TensorFlow 2.x makes the weights
     per channel: the server rearranges such layers so that they use the fast path of the NPU and stay exact (see below).
   * Float32 and float16 models run, by default on the CPU of the NPU die (see "How it works"); `device="npu"` puts them on the NPU (slower, some give wrong results).
   * `device="npu"` is strict: a model with an operator the NPU does not take is refused with its name (in our operator models: `SLICE`, `TILE`, `CEIL`, `ROUND`, `COS`,
     `LOG_SOFTMAX`, `TOPK_V2`, comparisons, logical operators, `CAST`); `"auto"` runs it.
     **Dynamic-range** models (int8 weights, float activations) are not supported on the NPU.
   * The operators the NPU runs: [OPERATORS.md](../npu-open/OPERATORS.md) (also in `/usr/share/doc/tinker-edge-r/`). What the NPU cannot do runs on the CPU of the NPU die
     (TensorFlow Lite with XNNPACK).
   * TensorFlow Lite 2.21 reads the models (LiteRT is the new name of TensorFlow Lite; its `.tflite` files and the classic interpreter API are what is used here).
   * ONNX or PyTorch models: convert to TFLite first (for example with `ai-edge-torch` or `onnx2tf`) and quantise as above. (Not part of the image.)
2. **Copy the file to the board and run it** with the system Python:
   ```python
   import numpy as np
   import rk3399pro_npu as npu

   net = npu.Model("mobilenet_v2_quant.tflite")          # first time: seconds to minutes; then 0.04-0.3 s (cache); attached if loaded already
   print(net.plan, net.inputs[0], net.outputs[0])        # where it runs; name, shape, dtype, scale, zero point
   x = np.zeros(net.inputs[0].shape, net.inputs[0].dtype)  # or a float array: it is quantised for you
   (scores,) = net.exec(x)                               # a list of numpy arrays, one per output
   print(net.outputs[0].dequantize(scores).argmax(), net.last_invoke_us, "us on the NPU die")
   net.close()                                           # this program lets go; the model stays loaded (net.stop() unloads it)

   report = npu.Model("other.tflite").autotune(sample)   # once per model: the fastest plan with the same result is kept
   npu.Model("net.tflite", device="npu")                # strictly on the NPU (NpuError if not possible); device="cpu": strictly on the CPU
   print(npu.models())                                   # the loaded models: id, name, plan, memory, runs
   npu.stop("all")
   ```
   Command line: `npu-info` (chip, driver, memory, number of models), `npu-models` (list), `npu-models stop ID|NAME|all`, `npu-selftest` (checks everything with two small models).
3. **From C++**: `#include <rk3399pro_npu.hpp>`, link with `-lrk3399pro_npu`; the same functions (`rk3399pro_npu::Model`, `exec`, `autotune`, `models()`, `stop()`).

## Numbers (one Tinker Edge R)

Our own models (release 0.4 numbers, the same on 0.4.1):

| Model (224x224 unless noted) | Type | On the NPU | `exec` from Python |
|---|---|---|---|
| MobileNet V2 | uint8 (TF Hub) | 6-7 ms | 9-11 ms (about 110 FPS) |
| MobileNet V2 | int8, per-channel (TensorFlow 2.x) | 27 ms | 38 ms |
| ResNet50 | int8, per-channel | 70 ms | 81 ms |
| Inception V3 (299) | int8, per-channel | 52 ms | 69 ms |
| a small test model (`npu-selftest`) | uint8 | 0.45 ms | 1.5 ms through the daemon (0.4: 1.8 ms) |

[LiteRT community vision models](https://huggingface.co/collections/litert-community/vision-models) (2026-10-27, random input of the model's shape; the reference is the
same model on the CPU of the NPU die, TensorFlow Lite 2.21 with XNNPACK on two Cortex-A35; "cos" is the lowest cosine similarity of the outputs to it; `exec` times with the USB transfer):

| Task | Model | Type | NPU (`device="npu"`) | CPU of the NPU die | cos on the NPU | `device="auto"` / `autotune()` |
|---|---|---|---|---|---|---|
| classification | ResNet18 | int8 | **19 ms** | 532 ms | 0.993 | NPU / NPU |
| classification | SqueezeNet 1.1 | int8 | **9.7 ms** | 112 ms | 0.999 | NPU / NPU |
| classification | MobileNet V3 large | int8 | 70 ms | 98 ms | 0.974 | NPU / CPU (the NPU result is below 0.99) |
| classification | EfficientNet-B0 | int8 | 742 ms | 364 ms | 0.973 | NPU / CPU |
| detection | YOLOX-nano | float32 | 996 ms | 887 ms | 1.000 | CPU / CPU |
| detection | EfficientDet-Lite0 | float32 | 216 ms | 427 ms | 0.40 | CPU |
| face detection | YuNet | float16 | 1.6 s | 0.75 s | 1.000 | CPU |
| segmentation | LR-ASPP MobileNet V3 | float32 | 4.9 s | 2.0 s | 1.000 | CPU |
| segmentation | MediaPipe selfie (multiclass) | float32 | 5.4 s | 3.0 s | 0.997 | CPU |
| segmentation | PIDNet-S | float32 | 36 s | 15 s | 0.00 | CPU |
| depth | MiDaS small | float16 | 7.8 s | 3.8 s | 1.000 | CPU / CPU |
| OCR (text detection) | PP-OCRv5 det | float16 | 14.5 s | 5.9 s | 0.38 | CPU |
| enhancement (x4) | Real-ESRGAN x4v3 | float32 | 33 s | 16 s | 0.00 | CPU |

What this says: **integer models belong on the NPU** (ResNet18 27 times faster than the CPU of the NPU die); float models run faster and right on that CPU. Some int8 models lose
accuracy on the NPU (MobileNet V3 and EfficientNet use hard-swish / sigmoid gates); `autotune()` notices it. Hand over integer arrays when speed matters: quantising a float input
in numpy costs about 8 ms for a 224x224x3 frame on this CPU. Compiling a model for the NPU takes seconds (SqueezeNet 4.5 s) to minutes (MobileNet V3 int8 106 s, EfficientNet 147 s);
a whole-NPU graph with integer inputs and outputs is cached and loads in 0.04-0.3 s (models whose inputs or outputs are float, as `ai-edge-torch` writes them, are compiled at
every load). The power profile (`tinker-power`) changes the host, not the NPU.

### Why integer models, and what the server does to per-channel ones

Two facts of this NPU found with the board: it runs convolutions with **per-tensor** weights on its fast integer path and with per-channel weights on a slow one (134 ms for MobileNet V2),
and it returns wrong numbers for **signed** int8 tensors. The delegate hands int8 over as uint8 (the same values shifted by 128). A per-channel layer is rewritten by the server before the model is
built (`npu-open/server/split_per_channel.h`): channels are grouped so that the scales of a group differ by at most 4 times, each group becomes a convolution with per-tensor weights, the outputs
are added. That is exact; it costs 2-3 convolutions per layer (27 ms instead of 7 ms for MobileNet V2) and keeps the accuracy that one scale per layer destroys. `NPU_SPLIT=0` in the environment of the server switches it off.

## ADB on the NPU die

The stock NPU firmware enables ADB next to the RKNN link (`/etc/init.d/.usb_config` has
`usb_adb_en` and `usb_ntb_en`). With the NPU running: `sudo apt install adb`,
`sudo adb shell` gives a root shell in the NPU's own Linux 4.4 (Buildroot, 2 GB RAM).

## Known problems

- **`async_exec` is experimental.** Sending a request right after a reply while others are in flight (pipelining) was seen to hang the USB transfer and, after the time-out, the
  xHCI of the board (`HC died`, the NPU gone until it is reloaded). A pause of 300 us before such a request avoids it in all tests (thousands of frames; the daemon makes the same
  pause when requests overlap), `exec` never hung. The real cause is not found. Recovery after a hang: `echo fe900000.usb > /sys/bus/platform/drivers/dwc3/unbind`, then `bind`,
  then `sudo systemctl restart npu-firmware`.
- The first load of a big model can take minutes (the graph is compiled on the two A35 of the NPU die); `autotune()` compiles every candidate once. A model that does not fit
  into the free memory of the NPU die is refused before it is sent.
- Float models on the NPU (`device="npu"`) are slow and some give wrong results (measured: PIDNet-S, Real-ESRGAN, PP-OCRv5 detection, EfficientDet-Lite0); the default puts float
  models on the CPU of the NPU die. Some int8 models are less accurate on the NPU (MobileNet V3, EfficientNet: cosine 0.97 to the CPU); `autotune()` finds that and picks another plan.
- Not working on the NPU: dynamic-range models, a graph that is all float16.
- The compiled graph of a model with float inputs or outputs (an int8 model with `QUANTIZE` / `DEQUANTIZE` at its ends, as `ai-edge-torch` writes them) is not cached: such a model is
  compiled at every load (seconds to minutes). Attaching to it while it is loaded is instant. Making the input and output integer avoids it.
- Models stay loaded until they are stopped (also when the program ends). Reloading the NPU firmware (`systemctl restart npu-firmware`, a reboot) unloads all of them.
- Anyone who can reach the daemon socket or open `/dev/bus/usb` of the NPU (group `plugdev`) can load and stop models of everyone and run code on the NPU die through a model; the NPU is not
  reachable from the network.
