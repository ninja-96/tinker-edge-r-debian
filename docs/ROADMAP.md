# Roadmap and status

State of every part of the board with this image (release 0.1). "Works" means verified on a
real board; the method is in the README status table and in [TESTING.md](TESTING.md).

## Works

| Area | Notes |
|---|---|
| Boot: DRAM blob, open SPL/U-Boot, extlinux, Linux 6.18 LTS | |
| Debian 13 rootfs, SSH, DHCP, mDNS, NTP, first-boot growth of the root filesystem | |
| eMMC in HS200, flashing with verification, kernel update on a running board | HS400 Enhanced Strobe gives CRC errors on this board, see the device tree comment |
| CPU with cpufreq, RAM | `fan53555` patch for the `vdd_cpu_b` / `vdd_gpu` regulators |
| GPU: OpenGL ES 3.1 (Panfrost), OpenCL 3.0 (Rusticl) | HDMI needed `cdn_dp` disabled, or no DRM card appeared |
| Hardware video: H.264 / VP8 / JPEG encode, H.264 decode through `mpp_service` | 720p H.264 encode 52 fps, decode 116 fps; the image boots the `mpp` device tree |
| GStreamer with `mpph264enc`, `mppvideodec`, ... | plugin built from a pinned commit |
| Wi-Fi (RTL8822CE, PCIe), Bluetooth (USB), RK809 audio, RTC | link-level checks only, see below |
| 40-pin header: GPIO, I2C6/7, SPI1/5, PWM0/1/3 | [GPIO.md](GPIO.md) |
| NPU (optional, closed stack) | power-up needs the reference clock at 24 MHz; [NPU.md](NPU.md) |

## Not tested yet (needs a person or hardware)

| Item | What to do |
|---|---|
| HDMI picture, HDMI audio | connect a monitor, run `scripts/test-board.sh`, look and listen |
| USB-A and USB-C with real devices, USB 3 speed | plug a USB 3 disk into every port, run the test |
| Boot from an SD card, the 2 GiB swap file | `scripts/flash-sd.sh`, boot |
| KDE Plasma desktop | install `task-kde-desktop` once HDMI works |
| Bluetooth pairing, Wi-Fi connection | pair a device, connect to an access point |
| Network throughput | `sudo apt install iperf3` on the PC, run the test |
| 40-pin electrical side | `GPIO_LOOPBACK` test with jumper wires, real I2C/SPI devices |
| Audio by ear | listen to the test tone at the 3.5 mm jack |

## Not supported

| Item | Why | What would fix it |
|---|---|---|
| DisplayPort over USB-C, USB-C device mode, SuperSpeed in both orientations | mainline `cdn-dp` and the Type-C PHY need an `extcon` that the mainline FUSB302 driver does not provide | glue driver from TCPM events to `extcon`, or the vendor `fusb30x` port ([OPEN.md](OPEN.md)) |
| Vulkan on the GPU | Mali-T860 (Midgard) is not covered by PanVK | none; only software `llvmpipe` |
| Mini PCIe LTE modem and SIM | the board has the slot and the SIM holder, no modem is fitted | fit a modem; enable the modem drivers; check slot power in the device tree |
| MIPI DSI display and MIPI CSI cameras | no panel or sensor is described in the device tree | [MIPI.md](MIPI.md) |
| Suspend/resume, boot logo | not implemented | |

## Planned

### Release 0.2

| Item | Notes |
|---|---|
| DRAM at 933 MHz | the Rockchip `rkbin` repository also has `rk3399pro_ddr_933MHz_v1.30.bin` (and 666 MHz); we use 800 MHz. About 16% more memory bandwidth if the memory is stable at 1866 MT/s. Test by flashing only the loader (`scripts/flash-at.sh 64 ...`), then run the memory stress test. Baseline at 800 MHz: `mbw` memcpy 3310 MiB/s on a big core, `sysbench` write 5530 MiB/s |

Looked at and left out of 0.2: the ALSA UCM profiles of `JeffyCN/mirrors` (branch `alsa-config`) are written
for the vendor codec driver (`Playback Path`, `Capture MIC Path`); the mainline RK809 has `Master Playback Volume`,
`Mic Capture Gain` and `Playback Mux` (HP / SPK, default HP), so the jack works without a profile. `libv4l-rkmpp`
does not build against Debian's `libv4l2` (its plugin interface lacks `mmap`; the project needs a patched
`v4l-utils`) and it is made for a Chromium built with the author's patches, so Debian's Chromium would not use it.
No browser is installed by the image.

### Release 0.3: NPU

| Item | Notes |
|---|---|
| NPU firmware ("the OS of the NPU die") | from `airockchip/RK3399Pro_npu` (Apache-2.0), unchanged, pinned commit and sha256, with a `NOTICE` that points to the sources of the GPL parts (Linux 4.4.185, BusyBox, galcore). Untested so far: the board was tested with the ASUS copy of the images |
| `npu_transfer_proxy`, `librknn_api.so` | from the same repository, unchanged; the proxy runs through `npu-proxy-run` |
| Open replacement for `upgrade_tool` | `rkdeveloptool` has `db` and `wl` but not `td` and `rs`; observe the USB protocol and implement them (or a small `libusb` loader) |
| Python 3.13 module for the host side | `ctypes` binding over `librknn_api.so` (init, query, inputs, run, outputs) with `numpy`, MIT; checked against the C demos and the `.rknn` models of the Apache repository |
| Model conversion | RKNN-Toolkit 1.7.x (BSD-3) only runs on Python 3.6-3.8 on x86: a Docker image for it; operator set is old (ONNX opset about 12) |
| Tests | `scripts/test-board.sh` gets an NPU phase |

### Release 0.3, optional artifact

| Item | Notes |
|---|---|
| Own Chromium build with `libv4l-rkmpp` | patched `v4l-utils` (mmap in the plugin interface) and Chromium with the V4L2 plugin patches from `JeffyCN/meta-rockchip`; a cross build on a PC (order of 100 GB of disk and hours of CPU), shipped as a separate download, not in the base image |

### Later

| Item | Notes |
|---|---|
| Open BL31 (TF-A) as the default | it builds (`USE_TFA=1`) but was never booted; test by flashing only the loader |
| Open DRAM initialisation | the U-Boot LPDDR4 timings of the Rock Pi 4 hang on this board; derive timings from the blob parameters |
| Inference engines on the GPU | CPU numbers for MobileNetV2: TFLite 33 FPS, ONNX Runtime 22, Arm NN 20, MNN 18, TVM untuned 1.4. Arm NN GpuAcc does not run on Rusticl (needs OpenCL 2.0). MNN, TVM and TFLite on the GPU need source builds. Expect little: a simple float4 kernel reaches 8 GFLOPS against a peak of about 100 |
| Open NPU stack | research only, [NPU-RESEARCH.md](NPU-RESEARCH.md): a newer kernel on the NPU die with `etnaviv` and Mesa Teflon |
| Next LTS kernel | bump `sources.lock`, rebase `patches/` and `kernel-mods/`, [KERNEL.md](KERNEL.md) |
| ffmpeg with `mpp` (ffmpeg-rockchip), 1080p numbers | |
