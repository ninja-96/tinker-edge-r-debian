# Tinker Edge R: Debian 13 on mainline Linux

Build and flash scripts for an open-source system on the **ASUS Tinker Edge R**
(Rockchip RK3399Pro): Debian 13 "trixie", Linux 6.18 LTS, mainline U-Boot, hardware video
encoding and decoding, GStreamer, OpenGL ES and OpenCL on the GPU. The stock ASUS system
has been stuck on Linux 4.4 since 2022; this is a way to run current software on the board.
Unofficial; not affiliated with ASUS or Rockchip.

Goal of this repository: a user with a board clones it, runs the commands below and
ends up with a working board (boots from the eMMC, network and SSH up, hardware video).

## Status

Legend. **Works**: yes / partly / no. **Tested on a board**: yes (by `scripts/test-board.sh` or by hand), partly
(only the software side or a short run), no (written, never run on hardware), needs you (needs a monitor, a
device or a person). Release 0.2 is published; items marked **0.3** are in the repository and in the 0.3 image, which
is being tested.

| Area | Works | Tested on a board | Details |
|---|---|---|---|
| Boot: DRAM init, U-Boot, extlinux, Linux 6.18.56 from the eMMC | yes | yes | many reboots; DRAM at 933 MHz ([fallback to 800](docs/BOOT.md)) |
| Flashing over USB, backup, restore, kernel update | yes | yes | read-back verification |
| Ethernet, SSH, mDNS name, NTP | yes | partly | link and names; about 500 Mbit/s measured on a 500 Mbit/s line, gigabit not measured |
| CPU (6 cores, cpufreq), RAM 4 GB | yes | yes | `stress-ng` 8 + 10 min, `memtester` full pass, 47 C at full load; 933 MHz not benchmarked against 800 |
| eMMC (HS200) | yes | yes | read 164 MB/s, write 26-52 MB/s; root grows to the whole eMMC on the first boot |
| GPU: OpenGL ES 3.1, OpenGL 3.1 (Panfrost), OpenCL 3.0 (Rusticl) | yes | yes | OpenCL kernels give correct results; Vulkan: no |
| Hardware video: H.264 / VP8 / JPEG encode, H.264 decode (`mpp`), GStreamer plugins | yes | yes | 720p H.264 encode 54 fps, decode 116 fps |
| HDMI picture | yes | yes | KDE Plasma on Wayland works; on X11 the frame rate drops from 60 to 30 while windows move |
| HDMI audio | not known | needs you | |
| Wi-Fi (RTL8822CE) | scan only | partly | access points are seen; connection and throughput not tested |
| Bluetooth | adapter only | partly | `hci0` up, scan finds a device; pairing not tested |
| Audio, 3.5 mm jack (RK809) | not known | partly | tone and recording run without errors; not listened to |
| RTC | yes | partly | write and read back; retention without power not tested |
| 40-pin header: GPIO, I2C6/7, SPI1/5, PWM0/1/3 | software side | partly | every plain GPIO pin driven and read; no loopback, no real devices ([GPIO.md](docs/GPIO.md)) |
| USB-A ports (keyboard, mouse) | yes | partly | a keyboard and a mouse work; a flash drive and USB 3 speed not tried |
| USB-C as host | not known | needs you | plug a device in; USB 3 speed |
| Boot from an SD card, swap file | not known | needs you | `scripts/flash-sd.sh` |
| **0.3** NPU: firmware load without closed tools, MobileNet from Python 3.13 | yes | yes (with the files of this build) | `rk3399pro_npu` module, 74 inferences/s; closed Rockchip files, fetched and checked by the build ([NPU.md](docs/NPU.md)); the 0.3 image itself not yet tested |
| **0.3** Converter ONNX to `.rknn` (Docker, Python 3.8, on the PC) | in progress | no | [docker/rknn-toolkit](docker/rknn-toolkit/README.md); ONNX opset 8-12 only |
| **0.3** Power profiles `tinker-power` | partly | no | tested on a fake sysfs only ([POWER.md](docs/POWER.md)) |
| **0.3** Firewall (nftables), forced password change | written | no | [SECURITY.md](docs/SECURITY.md) |
| DisplayPort over USB-C | **no** | | mainline has no Type-C DP glue for this SoC; the USB-C port is a USB host only ([OPEN.md](docs/OPEN.md)) |
| Vulkan | **no** | | Mali-T860 (Midgard) is not supported by PanVK; software `llvmpipe` only |
| GPU inference engines | **no** | | Arm NN GpuAcc does not run (needs OpenCL 2.0); MNN, TVM and TFLite on the GPU are untried; CPU numbers in [ROADMAP.md](docs/ROADMAP.md) |
| MIPI DSI display, MIPI CSI cameras | **no** | | drivers are built, no panel or camera is described in the device tree ([MIPI.md](docs/MIPI.md)) |
| Mini PCIe LTE modem, nano-SIM | **no** | | the slot is there, no modem is fitted, modem drivers are off |
| Suspend and resume, boot logo | **no** | | not implemented |
| Open DRAM init and open BL31 | **no** | | the image uses two closed Rockchip binaries; an open TF-A builds, never booted ([OPEN.md](docs/OPEN.md)) |

Details and plans: [docs/ROADMAP.md](docs/ROADMAP.md).

## What you need

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

## 2. Flash the eMMC over USB

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

## 3. First boot

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

## Using the hardware

```sh
# H.264 hardware encoding and decoding with GStreamer (the image boots the "full" device tree: mpp video and the NPU)
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
  ! mpph264enc ! h264parse ! filesink location=test.h264
gst-launch-1.0 filesrc location=test.h264 ! h264parse ! mppvideodec ! fpsdisplaysink video-sink=fakesink sync=false

eglinfo -p gbm -B | grep renderer       # Mali-T860 (Panfrost)
clinfo | grep -i "device name"          # Mali-T860 (Panfrost) through Rusticl, OpenCL 3.0
sudo gpioset -c gpiochip2 25=1          # 40-pin header, pin 7 (see docs/GPIO.md)
```

The default (`full`) and the `mpp` device trees turn off the mainline V4L2 decoders (`hantro`, `rkvdec`) because
`mpp_service` owns the same hardware; `sudo tinker-boot-select base` switches back
([docs/VIDEO.md](docs/VIDEO.md)).

## Desktop (KDE Plasma)

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

## Power profiles

`tinker-power` switches the CPU and GPU clocks between `powersave`, `normal` (default) and `performance`;
the choice survives a reboot: `sudo tinker-power performance`, see [docs/POWER.md](docs/POWER.md).

## Update the kernel

The kernel is built from the kernel.org tarball pinned in `sources.lock`. To move to a
new 6.18.x release: put the new version and checksum into `sources.lock`, rebuild with
`./run.sh scripts/build-kernel.sh` and install it on the running board without
reflashing with `scripts/update-kernel.sh`. Full instructions, including the next LTS
and how to rebase patches: [docs/KERNEL.md](docs/KERNEL.md).

## Test the board

`scripts/test-board.sh` runs a stress test over SSH (CPU, RAM, eMMC, GPU, OpenCL, video
codecs, network, USB, HDMI, audio, 40-pin header, RTC, Wi-Fi/Bluetooth, then everything at
once) and prints a PASS/FAIL table: [docs/TESTING.md](docs/TESTING.md).

## Open source policy

Only open source lives in this repository. The unavoidable closed binaries are
listed with their origin and licence in [docs/OPEN.md](docs/OPEN.md) and
[blobs/README.md](blobs/README.md): the Rockchip DRAM initialisation blob and BL31
(published unmodified under the Rockchip licence, with a link to where they come from),
and, for the optional NPU, the vendor stack, which is not in the repository.
The ASUS TinkerOS image and everything taken from it are never published.

## Repository layout

| Path | Contents |
|---|---|
| `scripts/` | build, flash and test scripts (`make-image.sh` is the entry point) |
| `docker/` | build environment (Debian 13, cross toolchain, QEMU) |
| `dts/` | device trees: base, `-mpp`, `-npu`, `-full` (default) |
| `patches/` | patches to Linux and TF-A |
| `kernel-mods/`, `u-boot-mods/` | new kernel and U-Boot files (`mpp_service` port, NPU power driver, board defconfig) |
| `config/` | kernel configuration fragment |
| `overlay/` | files copied into the rootfs (services, helper scripts) |
| `blobs/` | the Rockchip binaries, unmodified, with licence and origin |
| `third_party/` | `rkdeveloptool` (GPL), copied unmodified |
| `docs/` | documentation and research notes |
| `sources.lock` | pinned versions of the downloaded sources |

## Documentation

[KERNEL](docs/KERNEL.md) · [TESTING](docs/TESTING.md) · [VIDEO](docs/VIDEO.md) · [POWER](docs/POWER.md) · [SECURITY](docs/SECURITY.md) ·
[GPIO](docs/GPIO.md) · [MIPI](docs/MIPI.md) · [BOOT](docs/BOOT.md) ·
[FLASHING](docs/FLASHING.md) · [UART](docs/UART.md) · [HARDWARE](docs/HARDWARE.md) ·
[BRINGUP](docs/BRINGUP.md) · [NPU](docs/NPU.md) · [NPU-RESEARCH](docs/NPU-RESEARCH.md) ·
[OPEN](docs/OPEN.md) · [HISTORY](docs/HISTORY.md) · [ROADMAP](docs/ROADMAP.md) ·
[LINKS](docs/LINKS.md)

## Upstream and reference repositories

ASUS:
- https://github.com/TinkerEdgeR (Debian kernel 4.4 and U-Boot of the stock image:
  [debian-kernel](https://github.com/TinkerEdgeR/debian-kernel),
  [debian-u-boot](https://github.com/TinkerEdgeR/debian-u-boot),
  [ALPR](https://github.com/TinkerEdgeR/ALPR))
- https://github.com/TinkerBoard/TinkerBoard/wiki/Tinker-Edge-R (wiki)
- https://tinker-board.asus.com/download-list.html?product=tinker-edge-r (images and documents)

Rockchip:
- https://github.com/rockchip-linux/rkbin (source of `blobs/rockchip/`, commit `3e288fe814e059dd06833495f845cab04ac20a5c`)
- https://github.com/rockchip-linux/rkdeveloptool (source of `third_party/rkdeveloptool`, commit `304f073752fd25c854e1bcf05d8e7f925b1f4e14`)
- https://github.com/rockchip-linux/kernel (BSP kernel, donor of the `mpp_service` port)
- https://github.com/rockchip-linux/u-boot
- https://github.com/rockchip-linux/mpp (downloaded and built, Apache-2.0)
- https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip (GStreamer plugins, downloaded and built, LGPL-2.1)
- https://github.com/airockchip/RK3399Pro_npu , https://github.com/rockchip-linux/rknpu , https://github.com/rockchip-linux/rknn-toolkit (NPU)

Open upstreams the build downloads: https://www.kernel.org (Linux),
https://github.com/u-boot/u-boot, https://deb.debian.org (Debian). More references
in [docs/LINKS.md](docs/LINKS.md).

## Licenses

Scripts and docs: MIT. Kernel patches, drivers and device trees: GPL-2.0 (device
trees GPL-2.0+ OR MIT). Third-party components keep their own licenses; see
[NOTICE.md](NOTICE.md). The stock ASUS image and its binaries are **not** distributed here.
