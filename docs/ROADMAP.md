# Roadmap

| Area | State | Next |
|---|---|---|
| Boot (DRAM blob + open SPL/U-Boot, extlinux, Linux 6.18 LTS) | works | try the open BL31 (`USE_TFA=1`) |
| Debian 13 rootfs, SSH, DHCP, mDNS, NTP | works | - |
| Open DRAM init | hangs with Rock Pi 4 LPDDR4 timings | derive the timings (blob parameters with `ddrbin_tool.py`, coreboot values) |
| GPU: OpenGL ES 3.1, OpenCL 3.0 (Rusticl); no Vulkan on Midgard | works (eglinfo, OpenCL kernels checked) | - |
| GPU (panfrost, history) | needed the `fan53555` revision 0xc patch (`vdd_gpu` did not probe); fixed in the next image | verify with `scripts/test-board.sh` |
| HDMI | was blocked by the failing `cdn-dp` (no DRM card at all); `cdn_dp` disabled in the next image | verify with a monitor |
| USB-A ports, USB-C port (host mode, SuperSpeed in one orientation) | enabled, untested | plug devices into every port and run the test |
| DisplayPort over USB-C | not supported: mainline `cdn-dp` needs an extcon that reports DP, the mainline FUSB302 driver has none | port the vendor `fusb30x` extcon or wait for upstream |
| Hardware video decode (rkvdec/hantro) | enabled, untested | test with ffmpeg/GStreamer |
| Hardware video encode/decode (`mpp_service` port, default variant `-mpp`) + GStreamer plugin | works (H.264 720p encode 52 fps, decode 116 fps) | 1080p and VP8/JPEG numbers, ffmpeg-rockchip |
| Wi-Fi (RTL8822CE on PCIe) and Bluetooth (USB, firmware from Debian) | detected, firmware loads, `wlp1s0` and `hci0` exist | connection and throughput test |
| Audio (3.5 mm jack via RK809, HDMI) | RK809 card is registered; HDMI card needs a working display; nothing played yet | listen to the test tone |
| Boot from an SD card (2 GiB swap file on first boot) | untested | write with `scripts/flash-sd.sh` and boot |
| KDE Plasma desktop | instructions in the README, untested | install `task-kde-desktop` after HDMI works |
| NPU (variant `-npu`, closed stack) | works: power-up, firmware load, ADB, MobileNet inference | replace `upgrade_tool`, ship the Apache-licensed Rockchip files in `blobs/npu/`, see [NPU.md](NPU.md) |
| Open NPU kernel driver (port of NXP's Vivante driver) | research only | compare the ioctl ABI, see [NPU-RESEARCH.md](NPU-RESEARCH.md) |
| Next LTS kernel after 6.18 | - | bump `sources.lock`, rebase `patches/` and `kernel-mods/` |
| Inference engines on the GPU (MNN/TVM/TFLite with OpenCL) | CPU baselines measured (TFLite 33 FPS, ONNX Runtime 22, Arm NN 20, MNN 18 on MobileNetV2); Arm NN GpuAcc does not run on Rusticl; the other GPU paths need source builds | v0.2 or later |
