# Tinker Edge R: Debian 13 on mainline Linux

Build and flash scripts for an open-source system on the **ASUS Tinker Edge R**
(Rockchip RK3399Pro): Debian 13 "trixie", Linux 6.18 LTS, mainline U-Boot, hardware video
encoding and decoding, GStreamer, OpenGL ES and OpenCL on the GPU. The stock ASUS system
has been stuck on Linux 4.4 since 2022; this is a way to run current software on the board.
Unofficial; not affiliated with ASUS or Rockchip.

Goal of this repository: a user with a board clones it, runs the commands below and
ends up with a working board (boots from the eMMC, network and SSH up, hardware video).

## Status (release 0.2)

Checked on a real board with Linux 6.18.56 by `scripts/test-board.sh` (phases of 120 s, no failures) and by hand.
"Checked" says how far; things that need a person or extra hardware are listed separately.

### Works

| Area | What works | How far it was checked |
|---|---|---|
| Boot | DRAM init, U-Boot SPL and U-Boot, extlinux, Linux 6.18.56 from the eMMC | boots reliably, many reboots |
| Flashing | eMMC install over USB with read-back verification, backup and restore, kernel update on a running board | all used |
| Network | Gigabit Ethernet (DHCP), SSH, mDNS name `tinker-edge-r.local`, NTP | link and name resolution; throughput **not measured** |
| CPU, RAM | 6 cores (A53 up to 1.4 GHz, A72 up to 1.8 GHz) with cpufreq, 4 GB RAM at 933 MHz (LPDDR4-1866; ASUS used 800 MHz, [fallback](docs/BOOT.md)) | stress tests with result verification (`stress-ng` 8 + 10 min, `memtester` full pass, no errors), 47 C at full load; the 933 MHz setting was **not** benchmarked against 800 MHz |
| eMMC | 16 GB, HS200: read 164 MB/s, write 26-52 MB/s, root filesystem grows to the whole eMMC on first boot | 512 MiB write and read back with checksum |
| GPU | Mali-T860 with Panfrost: OpenGL ES 3.1 and OpenGL 3.1; **OpenCL 3.0** through Rusticl (on by default) | `eglinfo` shows Panfrost; OpenCL kernels give correct results; no image on a monitor yet |
| Hardware video | H.264 encode (720p at 54 fps), H.264 decode (116 fps), VP8 encode, JPEG encode and decode through Rockchip `mpp` (`/dev/mpp_service`) | encode and decode of a 60-frame clip, frame count and size checked |
| GStreamer | Debian GStreamer 1.26 with the Rockchip plugin: `mpph264enc`, `mppvp8enc`, `mppjpegenc`, `mppvideodec`, `mppjpegdec` | 300-frame 720p encode, decode at about 200 fps |
| Wi-Fi | Realtek RTL8822CE (PCIe), firmware from Debian | adapter up, scan sees access points; connection and throughput **not tested** |
| Bluetooth | adapter `hci0` (USB), firmware from Debian | adapter up, a scan finds a nearby device; pairing **not tested** |
| Audio | Codec RK809 (3.5 mm jack: playback and capture) | a test tone plays and a recording is made without errors; **not listened to** |
| RTC | RK809 clock `rtc0`, battery header `RTC_BATTERY` | write and read back; retention without power **not tested** |
| 40-pin header | GPIO (libgpiod), I2C6 and I2C7, SPI1 and SPI5 (`/dev/spidev*`), PWM0, PWM1, PWM3 | every plain GPIO pin driven and read, buses and device nodes present; **no electrical loopback or real devices** |
| NPU (optional) | power-up, firmware load, ADB shell inside the NPU, MobileNet inference through `librknn_api` on the mainline kernel | works, but needs closed files that are not in the image: [docs/NPU.md](docs/NPU.md) |

### Does not work or is not supported

| Area | State |
|---|---|
| **DisplayPort over USB-C** | not supported: mainline has no Type-C DP glue for this SoC ([docs/OPEN.md](docs/OPEN.md)). The USB-C port works as a USB host only, SuperSpeed in one plug orientation |
| **Vulkan** | none on the GPU: Mali-T860 (Midgard) is not supported by PanVK. Software `llvmpipe` only |
| GPU inference engines | Arm NN GpuAcc does not run (needs OpenCL 2.0); MNN, TVM and TFLite on the GPU are untried. CPU numbers are in [docs/ROADMAP.md](docs/ROADMAP.md) |
| MIPI DSI display, MIPI CSI cameras | kernel drivers are built, but no panel or camera is described in the device tree ([docs/MIPI.md](docs/MIPI.md)) |
| Mini PCIe slot with LTE modem and nano-SIM | the board has the slot, **no modem is fitted**; modem drivers are not enabled |
| Open DRAM initialisation, open BL31 | the image uses two closed Rockchip binaries ([docs/OPEN.md](docs/OPEN.md)); an open TF-A builds but is not tested |
| Suspend and resume, boot logo | not implemented |

### Not tested yet (needs a person or hardware)

HDMI picture on a monitor, HDMI audio, USB-A and USB-C with real devices and USB 3 speed,
boot from an SD card (and the 2 GiB swap file it creates), the KDE desktop below, Bluetooth
pairing, Wi-Fi connection, network throughput (`iperf3`), the 40-pin electrical side.
Each of these is a line in [docs/ROADMAP.md](docs/ROADMAP.md).

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
ssh debian@tinker-edge-r.local        # password: debian
```

If the name does not resolve, look for `tinker-edge-r` in the DHCP client list of
your router and use its IP. **Change the password right away** (`passwd`): SSH is
enabled and the default password is public.

Serial console (optional, for debugging): UART0 on the 40-pin header, pin 8 (board TX),
pin 10 (board RX), pin 9 (GND), 115200 8N1, 3.3 V. See [docs/UART.md](docs/UART.md).

The shell has Tab completion (`bash-completion`, case-insensitive, all matches on the first Tab)
and `htop` is installed.

Swap: none on the eMMC (it would wear the flash). On an SD card the first boot creates
a 2 GiB swap file.

## Using the hardware

```sh
# H.264 hardware encoding and decoding with GStreamer (the image boots the "mpp" device tree)
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
  ! mpph264enc ! h264parse ! filesink location=test.h264
gst-launch-1.0 filesrc location=test.h264 ! h264parse ! mppvideodec ! fpsdisplaysink video-sink=fakesink sync=false

eglinfo -p gbm -B | grep renderer       # Mali-T860 (Panfrost)
clinfo | grep -i "device name"          # Mali-T860 (Panfrost) through Rusticl, OpenCL 3.0
sudo gpioset -c gpiochip2 25=1          # 40-pin header, pin 7 (see docs/GPIO.md)
```

The `mpp` device tree turns off the mainline V4L2 decoders (`hantro`, `rkvdec`) because
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
| `dts/` | device trees: base, `-mpp` (default), `-npu`, `-full` |
| `patches/` | patches to Linux and TF-A |
| `kernel-mods/`, `u-boot-mods/` | new kernel and U-Boot files (`mpp_service` port, NPU power driver, board defconfig) |
| `config/` | kernel configuration fragment |
| `overlay/` | files copied into the rootfs (services, helper scripts) |
| `blobs/` | the Rockchip binaries, unmodified, with licence and origin |
| `third_party/` | `rkdeveloptool` (GPL), copied unmodified |
| `docs/` | documentation and research notes |
| `sources.lock` | pinned versions of the downloaded sources |

## Documentation

[KERNEL](docs/KERNEL.md) · [TESTING](docs/TESTING.md) · [VIDEO](docs/VIDEO.md) · [POWER](docs/POWER.md) ·
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
