<h1 align="center">🚀 Tinker Edge R: Debian 13 on mainline Linux</h1>

<p align="center">
<b>Give your ASUS Tinker Edge R a second life</b><br>
🐧 Debian 13 &nbsp;·&nbsp; ⚙️ Linux 6.18 LTS &nbsp;·&nbsp; 🥾 mainline U-Boot &nbsp;·&nbsp; 🎬 hardware video &nbsp;·&nbsp; 🎨 OpenGL ES + OpenCL &nbsp;·&nbsp; 🧠 NPU with LiteRT
</p>

Build and flash scripts for an open-source system on the **ASUS Tinker Edge R**
(Rockchip RK3399Pro). The stock ASUS system has been stuck on Linux 4.4 since 2022; this is a way to run
current software on the board.

**Unofficial**, not affiliated with **ASUS** or **Rockchip**.

Made for and tested on **board revision 1.02** (how to check yours: [docs/HARDWARE.md](docs/HARDWARE.md#board-revision)); on another revision USB-C may give no power.

🎯 **Goal:** a user with a board clones this repository, runs the commands below and ends up with a working board:
boots from the eMMC, network and SSH up, hardware video, the NPU, power profiles, a firewall.

## 📊 Status

|  | ✅ | ⚠️ | ❌ | ❔ |
|---|:---:|:---:|:---:|:---:|
| **Works** | yes | partly | no | not known |

|  | ✅ | 🟡 | ⬜ | 🙋 |
|---|:---:|:---:|:---:|:---:|
| **Tested on a board** | yes | partly: the software side or a short run | not yet | needs a person or a device |

| Area | Works | Tested on a board | Details |
|---|---|---|---|
| Boot: DRAM init, U-Boot, extlinux, Linux 6.18.59 from the eMMC | ✅ yes | ✅ yes | many reboots; DRAM at 933 MHz ([fallback to 800](docs/BOOT.md)) |
| Flashing over USB, backup, restore, kernel update | ✅ yes | ✅ yes | read-back verification |
| Ethernet, SSH, mDNS name, NTP | ✅ yes | ✅ yes | gigabit link; 500 Mbit/s measured, the limit of the internet line it was measured on |
| CPU (6 cores, cpufreq), RAM 4 GB | ✅ yes | ✅ yes | `stress-ng` 8 + 10 min, `memtester` full pass, 47 C at full load; 933 MHz not benchmarked against 800 |
| eMMC (HS200) | ✅ yes | ✅ yes | read 164 MB/s, write 26-52 MB/s; root grows to the whole eMMC on the first boot |
| GPU: OpenGL ES 3.1, OpenGL 3.1 (Panfrost), OpenCL 3.0 (Rusticl) | ✅ yes | ✅ yes | OpenCL kernels give correct results |
| Hardware video: H.264 / VP8 / JPEG encode, H.264 decode (`mpp`), GStreamer plugins | ✅ yes | ✅ yes | 720p H.264 encode 54 fps, decode 116 fps |
| HDMI picture | ✅ yes | ✅ yes | KDE Plasma on Wayland works; on X11 the frame rate drops from 60 to 30 while windows move |
| HDMI audio | ✅ yes | 🟡 partly | the stream runs; not heard yet (a monitor without speakers) |
| Wi-Fi (RTL8822CE) | ✅ yes | 🟡 partly | driver, firmware, the scan sees 2.4 and 5 GHz networks; **connect the two antennas** (U.FL on the module); connection and throughput with antennas not measured yet |
| Bluetooth | ✅ yes | 🟡 partly | pairing through KDE, PipeWire offers A2DP (SBC, SBC-XQ) and HFP (mSBC); without antennas the audio link drops; to be repeated with antennas |
| Audio, 3.5 mm jack (RK809) | ✅ yes | ✅ yes | headphones heard; the headset microphone works (it is the right channel: a mono `Headset microphone` source; a fix of the mainline codec driver, +30 dB) |
| RTC | ✅ yes | ✅ yes | keeps the time across reboots, set from NTP; without power it keeps the time only with a backup battery on the `RTC_BATTERY` header (without one it restarts at 2017-08-05) |
| 40-pin header: GPIO, I2C6/7, SPI1/5, PWM0/1/3 | ⚠️ software side | 🟡 partly | every plain GPIO pin driven and read; no loopback, no real devices ([GPIO.md](docs/GPIO.md)) |
| USB-A ports | ✅ yes | ✅ yes | all three ports at 5 Gbit/s (USB 3), a flash drive reads 94-96 MB/s in each; a keyboard and a mouse |
| USB-C port (devices plugged in: flash drive, hub) | ✅ yes, USB 2.0 | ✅ yes | flash drives and disks work at USB 2.0 speed (480 Mbit/s, 29-35 MB/s), in both plug orientations; the port gives 5 V to devices (FUSB302, TPS2553 switch, about 1.5 A); USB 3 over USB-C does not come up (to be looked at); board revision 1.02 ([HARDWARE.md](docs/HARDWARE.md#board-revision)) |
| Boot from an SD card, swap file | ❔ not known | 🙋 needs you | `scripts/flash-sd.sh` |
| NPU: LiteRT / TensorFlow Lite models (`.tflite`) from Python 3.13 and C++ (open stack: LiteRT 2.3.2 + TIM-VX + TFLite VX delegate on the NPU die, no RKNN) | ✅ yes | ✅ yes | `rk3399pro_npu.Model("net.tflite")`: uint8 MobileNet V2 6-7 ms on the NPU, int8 ResNet18 19 ms (27 times its CPU time), ResNet50 70 ms; several models at once, shared by several programs (`npu-daemon`), float models on the CPU of the NPU die by default, `autotune()` picks NPU / CPU / mixed per model; built from source, only Rockchip's firmware and the Vivante driver are closed ([NPU.md](docs/NPU.md), [operators](npu-open/OPERATORS.md)); the NPU is on by default (`full` device tree) |
| Power profiles `tinker-power` (powersave / normal / performance) | ✅ yes | ✅ yes | applied at boot, survive a reboot; CPU and GPU clocks only, memory stays at 933 MHz ([POWER.md](docs/POWER.md)) |
| Firewall (nftables), forced password change at the first login | ✅ yes | ✅ yes | only SSH, ping, IPv6 discovery, DHCP and mDNS come in; no measurable effect on throughput ([SECURITY.md](docs/SECURITY.md)) |
| Python for the header: libgpiod, smbus2, spidev | ✅ yes | 🟡 partly | import and a pin read tested; run with `sudo`; examples in [GPIO.md](docs/GPIO.md) |
| Boot logo | ✅ yes | ⬜ not yet | the white Debian swirl centred on black in U-Boot, the kernel and Plymouth, at any resolution |
| Suspend and resume | ⚠️ partly | 🟡 partly | deep sleep and an RTC wake-up work, the NPU is put to sleep with the board and keeps its models; a USB keyboard or mouse does not wake the board |
| DisplayPort over USB-C | ❌ no | | mainline has no Type-C DP glue for this SoC; the USB-C port is a USB host only ([OPEN.md](docs/OPEN.md)) |
| GPU inference engines | ❌ no | | Arm NN (`CpuAcc`, `GpuAcc`) is planned ([ROADMAP.md](docs/ROADMAP.md)) |
| MIPI DSI display, MIPI CSI cameras | ❌ no | | drivers are built, no panel or camera is described in the device tree ([MIPI.md](docs/MIPI.md)) |
| Mini PCIe LTE modem, nano-SIM | ❌ no | | the slot is there, no modem is fitted, modem drivers are off |
| Open DRAM init and open BL31 | ❌ no | | the image uses two closed Rockchip binaries; an open TF-A builds, never booted ([OPEN.md](docs/OPEN.md)) |

Details and plans: [docs/ROADMAP.md](docs/ROADMAP.md).

## 🧰 What you need

- A Linux PC (x86_64) with **Docker** (your user in the `docker` group), `git`,
  `usbutils`, `sudo`, about 30 GB of free disk and internet access (Linux, U-Boot and
  Rockchip `mpp` sources, Debian packages).
- The board with its **12-19 V DC power adapter**, a **USB-C cable with data**,
  a jumper or tweezers for header **J3 (Recovery)**, an **Ethernet** cable.
- Recommended: a **3.3 V USB-UART** adapter for the serial console.

## 1. Build the image (about 1 hour the first time)

```sh
git clone https://github.com/ninja-96/tinker-edge-r-debian.git
cd tinker-edge-r-debian
scripts/make-image.sh
```

Result: `out/tinker-edge-r-debian13.img` (about 1.9 GB) and the flashing tools in `out/tools/`.
The script downloads the Linux and U-Boot sources and Debian packages, and builds
everything inside a Docker container. Nothing from ASUS is downloaded and no Rockchip
binary is downloaded: the few Rockchip binaries needed are in [`blobs/`](blobs/README.md).
The Rockchip downloads are open-source code only: the `mpp` video library (Apache-2.0) and
the GStreamer plugin (LGPL-2.1), pinned in `sources.lock`.

A prebuilt image is attached to the [release](https://github.com/ninja-96/tinker-edge-r-debian/releases)
as `tinker-edge-r-debian13.img.xz` with a `sha256` file. To use it instead of building:

```sh
sha256sum -c tinker-edge-r-debian13.img.xz.sha256
mkdir -p out && xz -dk tinker-edge-r-debian13.img.xz && mv tinker-edge-r-debian13.img out/
./run.sh scripts/build-flash-tools.sh        # only the small USB flashing tools (a few minutes)
```

then continue with step 2.

## ⚡ 2. Flash the eMMC over USB

The eMMC is written through the SoC's USB boot mode (maskrom). This **erases the
system on the eMMC**, so a backup comes first.

1. Power off the board. Connect the board's **USB-C port** to the PC (USB 2.0 port,
   no hub). **Short header J3 (Recovery)** with a jumper, then connect the power adapter.
2. Check that the PC sees the board, then **remove the jumper**:
   ```sh
   lsusb -d 2207:        # "RK3399 in Mask ROM mode"
   ```
3. Back up the original eMMC (15 GB, takes a few minutes) and flash the image:
   ```sh
   scripts/backup-emmc.sh emmc-backup.img
   scripts/flash-emmc.sh --backup emmc-backup.img
   ```
   Type `ERASE-EMMC` when asked. The script writes the image in pieces, reads the eMMC
   back and compares it, and resets the board only if everything matches.
4. Disconnect the USB-C cable (J3 stays open) and power-cycle the board.

Optional, once, so that the tools work without `sudo` for USB access:
`sudo cp third_party/rkdeveloptool/99-rk-rockusb.rules /etc/udev/rules.d/ && sudo udevadm control --reload`.

Restore the original system any time:
`scripts/flash-emmc.sh --skip-backup-check emmc-backup.img` (in maskrom mode).
Details and pitfalls: [docs/FLASHING.md](docs/FLASHING.md).

## 🏁 3. First boot

Connect the Ethernet cable and wait **about two minutes** (the first boot grows the
root filesystem to the whole eMMC and creates the SSH host keys). Then:

```sh
ssh debian@tinker-edge-r.local        # password: debian; the first login asks for a new one
```

If the name does not resolve, look for `tinker-edge-r` in the DHCP client list of
your router and use its IP. The first login forces a password change (SSH is
enabled and the default password is public). A firewall lets in only SSH, ping and mDNS, see [docs/SECURITY.md](docs/SECURITY.md).

Serial console (optional, for debugging): UART0 on the 40-pin header, pin 8 (board TX),
pin 10 (board RX), pin 9 (GND), 115200 8N1, 3.3 V. See [docs/UART.md](docs/UART.md).

The shell has Tab completion (`bash-completion`, case-insensitive, all matches on the first Tab)
and `htop` is installed.

Swap: none on the eMMC (it would wear the flash). On an SD card the first boot creates
a 2 GiB swap file.

## 🔌 Using the hardware

```sh
# H.264 hardware encoding and decoding with GStreamer (the image boots the "full" device tree: mpp video and the NPU)
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
  ! mpph264enc ! h264parse ! filesink location=test.h264
gst-launch-1.0 filesrc location=test.h264 ! h264parse ! mppvideodec ! fpsdisplaysink video-sink=fakesink sync=false

eglinfo -p gbm -B | grep renderer       # Mali-T860 (Panfrost)
clinfo | grep -i "device name"          # Mali-T860 (Panfrost) through Rusticl, OpenCL 3.0
sudo gpioset -c gpiochip2 25=1          # 40-pin header, pin 7 (see docs/GPIO.md)

npu-info                                # the NPU: chip, driver, free memory, loaded models
npu-selftest                            # 17 checks with two small TensorFlow Lite models
npu-models                              # the models loaded on the NPU; npu-models stop ID|NAME|all
```

TensorFlow Lite on the NPU from Python (C++: `#include <rk3399pro_npu.hpp>`, `-lrk3399pro_npu`), details in [docs/NPU.md](docs/NPU.md):

```python
import numpy as np, rk3399pro_npu as npu
net = npu.Model("mobilenet_v2_quant.tflite")      # device="auto": NPU + CPU; "npu" / "cpu": strictly one of them
(scores,) = net.exec(np.zeros(net.inputs[0].shape, net.inputs[0].dtype))
net.autotune()                                     # once: the fastest plan with the same result, kept for the next loads
net.close()                                        # the model stays loaded for other programs; net.stop() unloads it
```

The default (`full`) and the `mpp` device trees turn off the mainline V4L2 decoders (`hantro`, `rkvdec`) because
`mpp_service` owns the same hardware; `sudo tinker-boot-select base` switches back
([docs/VIDEO.md](docs/VIDEO.md)).

## 🖥️ Desktop (KDE Plasma)

The image is a server system without a graphical interface. To install the KDE Plasma
desktop, connect a monitor to **HDMI**, log in over SSH (or on the serial console) and run:

```sh
sudo apt update
sudo apt install task-kde-desktop      # about 3 GB download, needs the 15 GB of the eMMC
sudo reboot
```

The login screen (SDDM) appears on the monitor. The GPU driver is Mesa Panfrost (OpenGL ES 3.1
and OpenCL 3.0 on the Mali-T860, no hardware Vulkan). The board has 4 GB of RAM; if the
desktop feels tight, enable compressed swap in RAM: `sudo apt install systemd-zram-generator`.
Sound goes to the 3.5 mm jack and, with a monitor attached, to HDMI.
To go back to the text console: `sudo systemctl set-default multi-user.target`.
The desktop is **not tested on this board yet**, see [docs/ROADMAP.md](docs/ROADMAP.md).

## 🔋 Power profiles

`tinker-power` switches the CPU and GPU clocks between `powersave`, `normal` (default) and `performance`;
the choice survives a reboot: `sudo tinker-power performance`, see [docs/POWER.md](docs/POWER.md).

## 🔄 Update the kernel

The kernel is built from the kernel.org tarball pinned in `sources.lock`. To move to a
new 6.18.x release: put the new version and checksum into `sources.lock`, rebuild with
`./run.sh scripts/build-kernel.sh` and install it on the running board without
reflashing with `scripts/update-kernel.sh`. Full instructions, including the next LTS
and how to rebase patches: [docs/KERNEL.md](docs/KERNEL.md).

## 🧪 Test the board

`scripts/test-board.sh` runs a stress test over SSH (CPU, RAM, eMMC, GPU, OpenCL, video
codecs, network, USB, HDMI, audio, 40-pin header, RTC, Wi-Fi/Bluetooth, then everything at
once) and prints a PASS/FAIL table: [docs/TESTING.md](docs/TESTING.md).

## 🔓 Open source policy

Only open source lives in this repository. The unavoidable closed binaries are
listed with their origin and licence in [docs/OPEN.md](docs/OPEN.md) and
[blobs/README.md](blobs/README.md): the Rockchip DRAM initialisation blob and BL31
(published unmodified under the Rockchip licence, with a link to where they come from),
and, for the NPU, Rockchip's firmware with the Vivante driver (fetched and checked by the build, not stored here); the software above the driver is open (TIM-VX, VeriSilicon's TFLite delegate, TensorFlow Lite, our server, daemon, C++ library and Python module).
The ASUS TinkerOS image and everything taken from it are never published.

## 🗂️ Repository layout

| Path | Contents |
|---|---|
| `scripts/` | build, flash and test scripts (`make-image.sh` is the entry point) |
| `docker/` | build environments (Debian 13 builder with QEMU; Debian 11 cross toolchain for the NPU die) |
| `dts/` | device trees: base, `-mpp`, `-npu`, `-full` (default) |
| `patches/` | patches to Linux and TF-A |
| `kernel-mods/`, `u-boot-mods/` | new kernel and U-Boot files (`mpp_service` port, NPU power driver, board defconfig) |
| `config/` | kernel configuration fragment |
| `overlay/` | files copied into the rootfs (services such as `npu-firmware` and `npu-daemon`, helper scripts, `npu-info`, `npu-models`, `npu-selftest`) |
| `npu-open/`, `python/`, `tests/npu/` | the NPU stack: server for the NPU die, the board side (daemon, C++ library, Python binding), patches, tools, operator comparison; the Python package `rk3399pro_npu`; self-test models |
| `blobs/` | the Rockchip binaries, unmodified, with licence and origin |
| `third_party/` | `rkdeveloptool` (GPL), copied unmodified |
| `docs/` | documentation and research notes |
| `sources.lock` | pinned versions of the downloaded sources |

## 📚 Documentation

[KERNEL](docs/KERNEL.md) · [TESTING](docs/TESTING.md) · [VIDEO](docs/VIDEO.md) · [POWER](docs/POWER.md) · [SECURITY](docs/SECURITY.md) ·
[GPIO](docs/GPIO.md) · [MIPI](docs/MIPI.md) · [BOOT](docs/BOOT.md) ·
[FLASHING](docs/FLASHING.md) · [UART](docs/UART.md) · [HARDWARE](docs/HARDWARE.md) ·
[BRINGUP](docs/BRINGUP.md) · [RELEASING](docs/RELEASING.md) · [NPU](docs/NPU.md) · [NPU-RESEARCH](docs/NPU-RESEARCH.md) ·
[OPEN](docs/OPEN.md) · [HISTORY](docs/HISTORY.md) · [ROADMAP](docs/ROADMAP.md) ·
[LINKS](docs/LINKS.md)

## 🔗 Upstream and reference repositories

Everything the project builds, downloads or learned from, with the licence of each (SPDX id, link to the licence text in the upstream tree). The licences and
sources of what ends up in the images are in [NOTICE.md](NOTICE.md) and inside the image (`/usr/share/doc/tinker-edge-r/`); more links in [docs/LINKS.md](docs/LINKS.md).

**Built or downloaded by the build** (versions and commits are pinned in `sources.lock`):

| Source | Role | Licence |
|---|---|---|
| [kernel.org](https://www.kernel.org) / [torvalds/linux](https://github.com/torvalds/linux) | Linux 6.18 LTS, patched and built | GPL-2.0 WITH Linux-syscall-note ([COPYING](https://github.com/torvalds/linux/blob/HEAD/COPYING)) |
| [u-boot/u-boot](https://github.com/u-boot/u-boot) | the bootloader, built with our board files | GPL-2.0+ ([Licenses/](https://github.com/u-boot/u-boot/tree/HEAD/Licenses)) |
| [ARM-software/arm-trusted-firmware](https://github.com/ARM-software/arm-trusted-firmware) | open BL31 (`USE_TFA=1`, not yet tested on the board) | BSD-3-Clause ([license.rst](https://github.com/ARM-software/arm-trusted-firmware/blob/HEAD/license.rst)) |
| [rockchip-linux/mpp](https://github.com/rockchip-linux/mpp) | media library for the video codecs | Apache-2.0 OR MIT, one header GPL-2.0 ([LICENSES/](https://github.com/rockchip-linux/mpp/tree/HEAD/LICENSES)) |
| [JeffyCN/mirrors, branch gstreamer-rockchip](https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip) | GStreamer plugins for mpp | LGPL-2.0+ in the file headers ([COPYING](https://github.com/JeffyCN/mirrors/blob/gstreamer-rockchip/COPYING), the LGPL-2.1 text) |
| [rockchip-linux/rkdeveloptool](https://github.com/rockchip-linux/rkdeveloptool) | USB flashing tool; `third_party/rkdeveloptool` is a byte-identical copy of commit `304f0737`, plus our `rs` patch | GPL-2.0+ ([license.txt](https://github.com/rockchip-linux/rkdeveloptool/blob/HEAD/license.txt)) |
| [rockchip-linux/rkbin](https://github.com/rockchip-linux/rkbin) | DRAM initialisation blob and BL31, copied unmodified to `blobs/rockchip/` (commit `3e288fe8`) | Rockchip binary licence ([LICENSE](https://github.com/rockchip-linux/rkbin/blob/HEAD/LICENSE), also `blobs/rockchip/LICENSE`) |
| [airockchip/RK3399Pro_npu](https://github.com/airockchip/RK3399Pro_npu) | firmware of the NPU die (Linux 4.4.185, the Vivante driver and libraries), binary only; we change its device tree and ramdisk | Apache-2.0 ([LICENSE](https://github.com/airockchip/RK3399Pro_npu/blob/8858114555f1700e41e185b14ec3d266342f8059/LICENSE)); inside: GPL software, Vivante's closed libraries |
| [VeriSilicon/TIM-VX](https://github.com/VeriSilicon/TIM-VX) | graph library on top of the Vivante driver, `main` (commit in `sources.lock`) built with the SDK headers of tag `v1.1.32`, for the NPU die | MIT ([LICENSE](https://github.com/VeriSilicon/TIM-VX/blob/main/LICENSE)) |
| [VeriSilicon/tflite-vx-delegate](https://github.com/VeriSilicon/tflite-vx-delegate) | TensorFlow Lite delegate for TIM-VX, `main` (commit in `sources.lock`), with [our patch](npu-open/patches) | MIT ([LICENSE](https://github.com/VeriSilicon/tflite-vx-delegate/blob/main/LICENSE)) |
| [google-ai-edge/LiteRT](https://github.com/google-ai-edge/LiteRT) | LiteRT 2.3.2 (TensorFlow Lite as its `tflite/`), linked into our server on the NPU die | Apache-2.0 ([LICENSE](https://github.com/google-ai-edge/LiteRT/blob/main/LICENSE)) |
| [tensorflow/tensorflow](https://github.com/tensorflow/tensorflow) | the TensorFlow tree pinned by LiteRT (commit `5c0b7a59`), for the few files LiteRT takes from it | Apache-2.0 ([LICENSE](https://github.com/tensorflow/tensorflow/blob/master/LICENSE)) |
| [google/XNNPACK](https://github.com/google/XNNPACK) | CPU kernels of TensorFlow Lite on the NPU die | BSD-3-Clause ([LICENSE](https://github.com/google/XNNPACK/blob/master/LICENSE)) |
| [pytorch/cpuinfo](https://github.com/pytorch/cpuinfo), [google/pthreadpool](https://github.com/google/pthreadpool) | used by XNNPACK | BSD-2-Clause ([cpuinfo](https://github.com/pytorch/cpuinfo/blob/main/LICENSE), [pthreadpool](https://github.com/google/pthreadpool/blob/main/LICENSE)) |
| [Maratyszcza/FP16](https://github.com/Maratyszcza/FP16), [Maratyszcza/FXdiv](https://github.com/Maratyszcza/FXdiv), [google/farmhash](https://github.com/google/farmhash) | headers and code used by TensorFlow Lite and XNNPACK | MIT ([FP16](https://github.com/Maratyszcza/FP16/blob/master/LICENSE), [FXdiv](https://github.com/Maratyszcza/FXdiv/blob/master/LICENSE), [farmhash](https://github.com/google/farmhash/blob/master/COPYING)) |
| [abseil/abseil-cpp](https://github.com/abseil/abseil-cpp), [google/flatbuffers](https://github.com/google/flatbuffers), [google/gemmlowp](https://github.com/google/gemmlowp), [google/ruy](https://github.com/google/ruy), [jax-ml/ml_dtypes](https://github.com/jax-ml/ml_dtypes) | libraries of TensorFlow Lite | Apache-2.0 ([abseil](https://github.com/abseil/abseil-cpp/blob/master/LICENSE), [flatbuffers](https://github.com/google/flatbuffers/blob/master/LICENSE), [gemmlowp](https://github.com/google/gemmlowp/blob/master/LICENSE), [ruy](https://github.com/google/ruy/blob/master/LICENSE), [ml_dtypes](https://github.com/jax-ml/ml_dtypes/blob/main/LICENSE)) |
| [libeigen/eigen](https://gitlab.com/libeigen/eigen) | linear algebra of TensorFlow Lite | MPL-2.0 ([COPYING.MPL2](https://gitlab.com/libeigen/eigen/-/blob/master/COPYING.MPL2)) |
| [OouraFFT](https://github.com/petewarden/OouraFFT) (fft2d) | FFT of TensorFlow Lite (mirror of Takuya Ooura's code) | Ooura's licence: use, copy, modify, distribute ([readme2d.txt](https://github.com/petewarden/OouraFFT/blob/master/readme2d.txt), [home](http://www.kurims.kyoto-u.ac.jp/~ooura/fft.html)) |
| [protocolbuffers/protobuf](https://github.com/protocolbuffers/protobuf) | fetched by the TensorFlow Lite build (its tools), not linked into our binaries | BSD-3-Clause ([LICENSE](https://github.com/protocolbuffers/protobuf/blob/main/LICENSE)) |
| [Debian Open Use Logo](https://www.debian.org/logos/) | the boot logo (white swirl on black: U-Boot, kernel, Plymouth) | LGPL-3.0-or-later or CC-BY-SA-3.0 ([legal code](https://creativecommons.org/licenses/by-sa/3.0/legalcode)) |
| [pybind11](https://github.com/pybind/pybind11) (Debian package `pybind11-dev`) | the Python binding of our C++ library (headers compiled into `rk3399pro_npu._core`) | BSD-3-Clause ([LICENSE](https://github.com/pybind/pybind11/blob/master/LICENSE)) |
| [deb.debian.org](https://deb.debian.org) | Debian 13 packages, including the `non-free-firmware` ones (Wi-Fi, Bluetooth) | per package: `/usr/share/doc/<package>/copyright`, see the [Debian licence overview](https://www.debian.org/legal/licenses/) |

**Read for reference, not used by the build:**

| Source | What we took from it | Licence |
|---|---|---|
| [rockchip-linux/kernel](https://github.com/rockchip-linux/kernel) (branch `develop-6.12`) | BSP kernel, the donor of the `mpp_service` port and the device tree of the video blocks | GPL-2.0 ([COPYING](https://github.com/rockchip-linux/kernel/blob/develop-6.12/COPYING)); our port keeps the GPL-2.0+ OR MIT labels of the files |
| [rockchip-linux/u-boot](https://github.com/rockchip-linux/u-boot) | the vendor U-Boot, compared with mainline | GPL-2.0+ ([Licenses/](https://github.com/rockchip-linux/u-boot/tree/HEAD/Licenses)) |
| [TinkerEdgeR/debian-kernel](https://github.com/TinkerEdgeR/debian-kernel), [debian-u-boot](https://github.com/TinkerEdgeR/debian-u-boot) | ASUS kernel 4.4 and U-Boot of the stock image: the board's device tree and pin facts | GPL-2.0 ([COPYING](https://github.com/TinkerEdgeR/debian-kernel/blob/HEAD/COPYING)), GPL-2.0+ ([Licenses/](https://github.com/TinkerEdgeR/debian-u-boot/tree/HEAD/Licenses)) |
| [TinkerEdgeR/ALPR](https://github.com/TinkerEdgeR/ALPR), the [ASUS wiki](https://github.com/TinkerBoard/TinkerBoard/wiki/Tinker-Edge-R), the [ASUS downloads](https://tinker-board.asus.com/download-list.html?product=tinker-edge-r) | facts about the board; the stock image and the documents are **not** distributed | no licence stated, so all rights stay with ASUS: nothing is copied from them |
| [airockchip/rknpu](https://github.com/airockchip/rknpu), [rockchip-linux/rknn-toolkit](https://github.com/rockchip-linux/rknn-toolkit) | the RKNN stack of release 0.3 (not used since 0.4) | BSD-3-Clause for the repositories ([rknpu](https://github.com/airockchip/rknpu/blob/HEAD/LICENSE), [rknn-toolkit](https://github.com/rockchip-linux/rknn-toolkit/blob/HEAD/LICENSE)); the Toolkit packages themselves are Rockchip's closed binaries |

## 📜 Licenses

Scripts and docs: MIT. Kernel patches, drivers and device trees: GPL-2.0 (device
trees GPL-2.0+ OR MIT). Third-party components keep their own licenses; see
[NOTICE.md](NOTICE.md). The stock ASUS image and its binaries are **not** distributed here.
