# RK3399Pro NPU on the Tinker Edge R: TensorFlow Lite

> **Status (release 0.4): TensorFlow Lite models run on the NPU through an open software stack; RKNN is gone.** The NPU die runs Linux 4.4.185 and the
> Vivante driver from Rockchip's firmware (closed, Apache-2.0 repository `airockchip/RK3399Pro_npu`, used as it is except for one device tree
> change). Everything above the driver is open: [TIM-VX](https://github.com/VeriSilicon/TIM-VX), VeriSilicon's TFLite delegate for TensorFlow Lite 2.6.0,
> our small server on the NPU die, our USB channel and the Python module `rk3399pro_npu` on the board. The numbers below were measured on a board
> (release 0.4 image); details of the research behind them are in [NPU-RESEARCH.md](NPU-RESEARCH.md) and `npu-open/`.

## How it works

```
 board (RK3399Pro host, Debian 13)                         NPU die (2 x Cortex-A35, 2 GB, Linux 4.4.185 from RAM)
 Python 3: rk3399pro_npu.Model("net.tflite")  -- USB 3 -->  npu_server (our)  -->  child process "engine":
   own bulk channel (FunctionFS "ntb"),                       TensorFlow Lite 2.6 + VX delegate (VeriSilicon) -> TIM-VX -> libVSC/galcore (Rockchip)
   38-52 MB/s, 0.3-0.5 ms for a small message
```

* The model file stays on the board. `Model(path)` sends it to the server, which builds the graph for the NPU (**the first time takes seconds to a minute**,
  see below); the compiled graph is cached on the board (`~/.cache/npu-models`) and the next load takes **0.1-0.3 s**.
* `exec(x)` sends the input, the NPU runs, the output comes back. The model runs in a **child process** of the server: a model that crashes the
  delegate or the driver is answered with the reason (`NpuError`), and the NPU and the USB link stay up.
* **One model per NPU**; `close()` (or leaving the `with` block) unloads it. The server checks the chip identity (model 0x8000, revision 0x7100, product
  0x45080001 - every Tinker Edge R has it) and refuses to run on anything else, and checks that the NPU die has enough free memory.

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
| `npu_server`, `libvx_delegate.so`, `libtim-vx.so` (inside `boot.img`, run on the NPU die) | built from source by `scripts/build-npu-open.sh` (TIM-VX v1.1.32, tflite-vx-delegate tag `tflite_v2.6.0` with [our patch](../npu-open/patches), TensorFlow 2.6.0, our `npu-open/server/`), `-O3 -mcpu=cortex-a35`, in a Debian 11 container |
| `rkdeveloptool` with the `rs` command | `third_party/rkdeveloptool` plus `patches/rkdeveloptool/` (GPL-2.0+), built for arm64 by `scripts/build-rkdeveloptool-arm64.sh` |
| `npu-load`, `npu-firmware.service`, `60-tinker-npu.rules` | this repository, `overlay/` |
| `rk3399pro_npu` (Python module), `npu-info`, `npu-selftest` | this repository, `python/` and `overlay/` (MIT) |

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

Access: the NPU appears on USB as `2207:0019`; the udev rule `60-tinker-npu.rules` gives it to the group `plugdev`, so the default user needs no root.

## Run your own model

1. **Make a `.tflite` file on the PC** with any converter of TensorFlow (or another tool that writes TFLite). Rules that matter:
   * **Use an integer model** - full 8-bit quantisation (int8 or uint8, inputs and outputs too). `tf.lite.Optimize.DEFAULT` with a representative dataset (a few dozen real inputs),
     `target_spec.supported_ops = [TFLITE_BUILTINS_INT8]`, `inference_input_type = inference_output_type = tf.int8` (or `tf.uint8`). TensorFlow 2.x makes the weights per channel: that is
     fine, the server rearranges such layers so that they use the fast path of the NPU and stay exact (see below).
   * Float32 models run (the NPU does it in float, **about 480 ms for MobileNet V2**), and so do models with float16 weights (the same speed). **Dynamic-range** models (int8 weights,
     float activations) are not supported.
   * The operators the NPU runs are listed in [OPERATORS.md](../npu-open/OPERATORS.md) (also in `/usr/share/doc/tinker-edge-r/`). An unsupported operator makes the delegate leave it
     to the CPU of the NPU die (two Cortex-A35, slow, and the data leave the NPU); some models are refused. Operators that gave wrong results or crashes: `GATHER_ND`,
     `RESIZE_NEAREST_NEIGHBOR`, float `FULLY_CONNECTED` and `TRANSPOSE_CONV`.
   * TensorFlow Lite 2.6.0 reads the models: newer converters may write operator versions it does not know. Models made with TensorFlow up to 2.13 were loaded.
   * ONNX or PyTorch models: convert to TensorFlow/TFLite first (for example with `onnx2tf` or `ai-edge-torch`) and quantise as above. (Not part of the image.)
2. **Copy the file to the board and run it** with the system Python:
   ```python
   import numpy as np
   import rk3399pro_npu as npu

   with npu.Model("mobilenet_v2_quant.tflite") as net:      # first time: seconds to a minute; then 0.1-0.3 s (cache)
       print(net.inputs[0], net.outputs[0])                  # name, shape, dtype, scale, zero point
       x = np.zeros(net.inputs[0].shape, net.inputs[0].dtype)  # or a float array: it is quantised for you
       (scores,) = net.exec(x)                               # a list of numpy arrays, one per output
       print(net.outputs[0].dequantize(scores).argmax(), net.last_invoke_us, "us on the NPU")
   ```
   `npu-info` shows the state of the NPU (chip, driver, memory, whether a model is loaded); `npu-selftest` checks everything with two small models.

## Numbers (release 0.4, one Tinker Edge R)

| Model (224x224 unless noted) | Type | On the NPU | `exec` from Python |
|---|---|---|---|
| MobileNet V2 | uint8 (TF Hub) | 6-7 ms | 9-11 ms (about 110 FPS) |
| MobileNet V2 | int8, per-channel (TensorFlow 2.x) | 27 ms | 38 ms |
| ResNet50 | int8, per-channel | 71 ms | |
| Inception V3 (299) | int8, per-channel | 52 ms | |
| MobileNet V2 | float32 / float16 weights | 480 ms | |

Hand over integer arrays when speed matters: quantising a float input in numpy costs about 8 ms for a 224x224x3 frame on this CPU (uint8 MobileNet V2: 9-11 ms per `exec` with a uint8 input, 18 ms with a float one).

The first load of a model builds its graph on the NPU: 13 s for MobileNet V2 uint8, about a minute for an int8 model with per-channel weights; the cached graph loads in 0.1-0.3 s.
Accuracy: the same top-1 class as TensorFlow Lite on the CPU; cosine similarity of the outputs 0.999-1.000 (per-channel int8) to the float model.
The power profile (`tinker-power`) changes the host, not the NPU.

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

- **`async_exec` is experimental.** Sending a request right after a reply (pipelining) was seen to hang the USB transfer and, after the time-out, the xHCI of the board (`HC died`, the NPU
  gone until it is reloaded). A pause of 300 us before each request avoids it in all tests (thousands of frames), `exec` never hung. The real cause is not found. Use `exec`. Recovery after a
  hang: `echo fe900000.usb > /sys/bus/platform/drivers/dwc3/unbind`, then `bind`, then `sudo systemctl restart npu-firmware`.
- One model at a time; the first load of a big model can take minutes; a model that does not fit in the 2 GB of the NPU die is refused.
- Not working: dynamic-range models, a graph that is all float16 (TIM-VX crashes), models with dynamic shapes (Keras `Reshape` with a computed shape) are refused by the delegate.
- Killing a Python program that holds a model: the server notices the closed link on its own after the USB reset; if the NPU stays unresponsive, `sudo systemctl restart npu-firmware`.
- Anyone who can open `/dev/bus/usb` of the NPU (group `plugdev`) can run code on the NPU die through the model; the NPU is not reachable from the network.
