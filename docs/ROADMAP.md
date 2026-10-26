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
| USB-A with a flash drive, USB-C with a device, USB 3 speed | a keyboard and a mouse work on USB-A (checked by the user); plug a USB 3 disk into every port, run the test |
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
| `htop` and Tab completion in the shell | `htop` and `bash-completion` in the image; `/etc/inputrc` switches on case-insensitive completion, listing all matches on the first Tab and PageUp / PageDown history search (`scripts/stage-rootfs.sh`) |
| DRAM at 933 MHz | the Rockchip `rkbin` repository also has `rk3399pro_ddr_933MHz_v1.30.bin` (and 666 MHz); we use 800 MHz. About 16% more memory bandwidth if the memory is stable at 1866 MT/s. Test by flashing only the loader (`scripts/flash-at.sh 64 ...`), then run the memory stress test. Baseline at 800 MHz: `mbw` memcpy 3310 MiB/s on a big core, `sysbench` write 5530 MiB/s |

Looked at and left out of 0.2: the ALSA UCM profiles of `JeffyCN/mirrors` (branch `alsa-config`) are written
for the vendor codec driver (`Playback Path`, `Capture MIC Path`); the mainline RK809 has `Master Playback Volume`,
`Mic Capture Gain` and `Playback Mux` (HP / SPK, default HP), so the jack works without a profile. `libv4l-rkmpp`
does not build against Debian's `libv4l2` (its plugin interface lacks `mmap`; the project needs a patched
`v4l-utils`) and it is made for a Chromium built with the author's patches, so Debian's Chromium would not use it.
No browser is installed by the image.

### Release 0.3: NPU

| Item | Status |
|---|---|
| Rockchip NPU files, pinned commit and sha256 | done: `scripts/fetch-npu.sh`; verified on the board together (firmware, proxy 2.1.0, `librknn_api` 1.7.5) |
| Device tree of the NPU boot image | done: the Rockchip kernel cannot drive the board's regulator; `scripts/npu-fw-tinker.py` replaces it by a fixed one ([NPU.md](NPU.md)) |
| Open replacement for `upgrade_tool` | done: `rs` added to `rkdeveloptool` (`patches/rkdeveloptool/`); `npu-load` uses it. built for arm64 by `scripts/build-rkdeveloptool-arm64.sh` (needs the qemu binfmt handler on the build host); in the image |
| Python module for the host side | done: `python/rk3399pro_npu` (`ctypes` + `python3-numpy` from apt); in the image, NPU phase in `scripts/test-board.sh` passes |
| Model conversion ONNX to `.rknn` | done: `docker/rknn-toolkit` (Python 3.8 image with Rockchip's closed Toolkit downloaded at build time; ONNX opset 8-12); tested: converted and run on the NPU. The format is undocumented, an own converter is a research project |

### Release 0.3: power profiles

| Item | Notes |
|---|---|
| `tinker-power`: powersave / normal / performance (CPU and GPU clocks), saved and applied at boot | tested on the board (applied at boot, survives a reboot; the `test-board.sh` phase checks it); see [POWER.md](POWER.md). Memory clock is not included until the DMC driver (0.4) |

### Release 0.3: basic security

| Item | Status |
|---|---|
| `nftables` firewall, on by default | written (`overlay/etc/nftables.conf`, `nftables` in the image): input policy drop; loopback, established connections, SSH (22), ping, IPv6 discovery, DHCP and mDNS (UDP 5353) allowed; SSH itself unchanged. **Tested on the board**: closes other ports, lets SSH, ping, mDNS through, starts at boot; throughput with and without it is the same (about 400 Mbit/s with a Python test; `iperf3` gigabit not measured). The test found that the kernel had no `nf_tables`: fixed. See [SECURITY.md](SECURITY.md) |
| Forced password change | written (`chage -d 0 debian` in `scripts/build-rootfs.sh`, `FORCE_PW_CHANGE=0` turns it off). **Tested on the board** |

Cheap measures that were discussed and are **not** scheduled (pick any; SSH stays as it is by decision): kernel switches that Debian's kernel has
and ours lacks (`HARDENED_USERCOPY`, `FORTIFY_SOURCE`, `SLAB_FREELIST_RANDOM`, `YAMA`, `LOCKDOWN`, `LANDLOCK`;
`INIT_ON_ALLOC_DEFAULT_ON` costs 1-3% speed), AppArmor, `unattended-upgrades`, `sysctl` hardening (`kptr_restrict`, `dmesg_restrict`, syncookies), LLMNR off.
Not done on purpose: secure boot through eFuses (irreversible), a secure OS (OP-TEE / BL32 is not loaded;
TrustZone only hosts BL31 for PSCI).

### A browser with hardware video decoding (decided: Chromium is part of the image; KDE work is not in 0.3)

Why: on the KDE desktop the browser uses a lot of CPU and plays video in software (reported 2026-10-25).

What is known: Debian's arm64 Chromium is built with `use_v4l2_codec=true`, `use_vaapi=true` and
`proprietary_codecs=true` (from the Debian packaging, `debian/rules`), so the V4L2 and VA-API decoder code is compiled in.
What is not known: whether it drives the RK3399 decoders. Candidate paths, to be tried on the board in this order:

| Path | What it needs | Risk |
|---|---|---|
| A. Chromium's V4L2 stateless decoder on the mainline `rkvdec` (H.264, VP9) and `hantro` (VP8, MPEG-2) | the `base` device tree (the `mpp` variant turns these drivers off); Chromium feature flags; `/dev/video*` and `/dev/media*` permissions | the stateless path is built for ChromeOS; on desktop Linux it may not be reachable |
| B. VA-API through `libva-v4l2-request` (exposes the same stateless decoders as VA-API), used by Chromium (`use_vaapi`) and Firefox | the `base` device tree; a maintained fork of `libva-v4l2-request` built for arm64 | the project is old and unmaintained |
| C. Hybrid device tree | keep the mainline `rkvdec` for the browser and give only the encoder (VEPU) to `mpp_service`, so that browser decoding and GStreamer encoding both work | mpp then loses H.265/VP9 decoding through GStreamer; needs a new DTB variant |
| D. Own Chromium with `libv4l-rkmpp` | patched `libv4l2` and a Chromium with the author's patches (written for old Chromium versions, must be rebased) | cross build of order 100 GB and hours; last resort |

Plan: (1) run `scripts/diag-desktop.sh` to see whether KWin renders on Panfrost at all (a software compositor would explain
the CPU load by itself); (2) install `chromium` on the board and test A and B with the `base` device tree, judging by
`chrome://gpu` and `chrome://media-internals`; (3) decide between C and D only if A and B fail; (4) put the working
browser and its configuration (flags in `/etc/chromium.d/`) into the image, with a test in `scripts/test-board.sh` (a clip
that must be played with hardware decoding). Image size grows by about 300 MB (`ROOTFS_MIB`).

### Release 0.4: memory controller

| Item | Notes |
|---|---|
| DMC: DRAM frequency scaling (`CONFIG_ARM_RK3399_DMC_DEVFREQ`, `rockchip,rk3399-dmc`) | saves power and heat at idle. Switched off today (`config/linux-tinker.fragment`) because a failing frequency change hangs the board. Needs: a device tree node with an OPP table and the DFI monitor, DRAM timings for every step (the Rockchip blob reports "dts ddr timing unavailable, use default"), and BL31 that handles the frequency change calls (the Rockchip BL31 does; the open TF-A has the code too). Test with the serial console attached and a way back (maskrom), under a memory stress test |
| NoC QoS priorities | the vendor kernel raises the priority of the display controller so that a 4K picture does not tear under heavy memory traffic; mainline does not. Check with HDMI at 4K under load once a monitor is available |

### Release 0.4: inference engines on the CPU and the GPU

Goal: a user converts or takes a model and runs it on the board with the system Python (3.13), on the CPU and on
the GPU, from files that are in the image or one download away. Converters work on x86_64 and, where possible,
arm64. The order is fixed: **Arm NN, TFLite, MNN, TVM**; each engine gets its own build script, a Python
binding for the system Python, a phase in `scripts/test-board.sh` (the same model gives the same result on CPU
and GPU within a tolerance, FPS reported) and a page in the documentation.

Earlier CPU numbers (MobileNetV2, pip builds, [INFERENCE-BENCH](INFERENCE-BENCH.md) in the working copy): TFLite 33 FPS, ONNX Runtime 22,
Arm NN 20, MNN 18, TVM untuned 1.4. Builds in 0.4 use `-mcpu=cortex-a72.cortex-a53 -O3`.

| Order | Engine | CPU | GPU | Notes |
|---|---|---|---|---|
| 1 | **Arm NN** (the official runtime for Arm Mali) | `CpuAcc` with the Compute Library | `GpuAcc` | **GPU is the known obstacle**: it failed on Rusticl with "Non uniform workgroup size is not supported": Rusticl on Midgard (Mali-T860) offers OpenCL C 1.2 only, the Compute Library wants OpenCL 2.0 features. To investigate: Compute Library settings or patches for uniform work-group sizes, a newer Mesa. If it cannot be done the reasons are written down. Binding: PyArmNN built with SWIG for Python 3.13 if the current Arm NN still has it, otherwise a `ctypes` binding. Models: Arm NN's parsers (ONNX, TFLite) read them at run time; an optional PC-side step can write the optimised network |
| 2 | **TFLite** | XNNPACK | GPU delegate | the GPU delegate can use OpenGL ES 3.1 compute shaders, which Panfrost has; needs a source build of the delegate and the Python `tflite_runtime` for 3.13. Models: `.tflite` (conversion from ONNX or TensorFlow on the PC) |
| 3 | **MNN** | CPU backend | OpenCL backend | pip's MNN has no OpenCL (it silently falls back to the CPU): source build with `MNN_OPENCL=ON`, Python module for 3.13. Model: `.mnn` from the MNN converter (x86_64 and arm64) |
| 4 | **TVM** | LLVM target for the A72/A53 | OpenCL target (`opencl`/`mali`) | untuned CPU result was 1.4 FPS: autotuning is part of the job (done on the board or with RPC); Python package for 3.13 |

Expect little from the GPU in every engine: a plain float4 OpenCL kernel reached about 8 GFLOPS of a peak near 100,
and the GPU shares the memory bandwidth with the CPU, so the CPU may stay faster for small models. Whatever the
measurement says goes into the tables in the documentation. NCNN has no row: it needs Vulkan, which this GPU lacks.

### Planned: an open NPU stack on VeriSilicon's TIM-VX (replaces RKNN)

Decision (2026-10-26): RKNN goes out of the product once this path has caught up. Why it can: the Vivante driver and libraries in
Rockchip's NPU firmware (SDK 6.4.6) run **TIM-VX v1.1.32** (the same SDK version; all 104 of its unit tests pass on the NPU) and the
**TFLite VX delegate**: MobileNet V2 (uint8) takes 6.5 ms per inference, RKNN is about as fast. Only the closed driver stays; the model format,
the converter and the server become open and standard (`.tflite` from TensorFlow, MIT code). Research and numbers: `rnd/verisilicon/`.

| Item | Notes |
|---|---|
| One chip | decision (2026-10-26): every Tinker Edge R has the same NPU, model 0x8000 revision 0x7100 product 0x45080001 (the only chip that Rockchip's and ASUS's `galcore` know); the build is made for it only. The server reads the identity from sysfs at the start and refuses to run on anything else, with a clear message; `test-board.sh` prints it |
| Transport: the `ntb` USB function | the gadget of the NPU die already has a FunctionFS function `ntb` (two bulk endpoints, 1024-byte packets, SuperSpeed) next to `adb`; today `rknn_server` opens it and `npu_transfer_proxy` talks to it with libusb. Our server can take it over (write the descriptors to `/dev/usb-ffs/ntb/ep0`, then use `ep1` and `ep2`) and the host client talks bulk USB with `libusb` (`ctypes`, no ADB, no gadget change). **Measured (2026-10-26):** stock ADB gives 5-6 MB/s and 1.3 ms for 1 KB (even through `adb forward`); our own bulk channel on the `ntb` function gives 38 MB/s each way for a 150 KB frame (3.9 ms round trip) and 52 MB/s for 1 MiB (0.3-0.5 ms for small messages). Rules found: write the FunctionFS descriptors before the USB controller is bound (our own `S50usbdevice` in the NPU ramdisk, with a fallback to `rknn_server`), read OUT endpoints in multiples of 1024 bytes, give the interface a class that ADB does not take |
| Compiled graph (NBG) | TIM-VX can export a compiled graph (`Graph::CompileToBinary`) and run it (the NBG operation); the unit tests of the export pass on the NPU. The TFLite delegate (needed once, to build the graph from a `.tflite` on the NPU) does not cache by itself; the server would then run NBG only, without TFLite |
| Thin server on the NPU die | accepts a model and prepares the graph, runs inputs, one model at a time (an error if the memory is not enough or another model is loaded), frees it on request; the models live on the host |
| Host API (Python, system 3.13) | a class: the constructor loads and prepares the model, `exec(x)` runs synchronously, `async_exec(x, callback)` runs asynchronously and calls the callback with the result, `del` unloads the model and frees the NPU |
| Transport over USB | to be measured: ADB (about 5 MB/s with `adb push`) against a USB network function or raw bulk; 224x224x3 at 155 FPS is 23 MB/s |
| Load time | the first inference takes 13 s (the Vivante compiler builds the graph on the two A35 cores): use the delegate's cache of the compiled graph (NBG), kept on the host |
| Data types | uint8 is fast; float32 is 74 times slower (478 ms); int8, int16 and float16 still to measure |
| Versions | TIM-VX v1.1.57 and v1.1.88 and a newer TensorFlow Lite on the driver 6.4.6: to try (the unit tests decide) |
| Removal of RKNN | `rknn_server` (it stays in the firmware unused), `npu_transfer_proxy`, `librknn_api`, the RKNN-Toolkit converter in Docker and the `rk3399pro_npu` module go once the new path has parity (speed, accuracy, tests) |

### Later

| Item | Notes |
|---|---|
| Open BL31 (TF-A) as the default | it builds (`USE_TFA=1`) but was never booted; test by flashing only the loader |
| Open DRAM initialisation | the U-Boot LPDDR4 timings of the Rock Pi 4 hang on this board; derive timings from the blob parameters |
| Inference engines on the GPU | CPU numbers for MobileNetV2: TFLite 33 FPS, ONNX Runtime 22, Arm NN 20, MNN 18, TVM untuned 1.4. Arm NN GpuAcc does not run on Rusticl (needs OpenCL 2.0). MNN, TVM and TFLite on the GPU need source builds. Expect little: a simple float4 kernel reaches 8 GFLOPS against a peak of about 100 |
| Open NPU stack | research only, [NPU-RESEARCH.md](NPU-RESEARCH.md): a mainline kernel (6.7 or newer) on the NPU die with the `etnaviv` driver and Mesa's Teflon delegate for TensorFlow Lite. Teflon supports VeriSilicon VIPNano-QI.7120 (Amlogic A311D) and VIPNano-SI+.8002 (NXP i.MX8M Plus); MobileNet V1 takes about 6.6 ms there, about 5.3 ms on this NPU with RKNN. First step done (2026-10-26): the chip of this NPU is model 0x8000, revision 0x7100, product 0x45080001 (sysfs `info` of `galcore`); mainline `etnaviv_hwdb.c` has 0x8000 only with revisions 0x6205, 0x7120 (A311D, product 0x45080009) and 0x8002, so this chip is **not in the table, but next to the A311D one**: an entry would be a small patch if the feature bits can be taken from the table inside Rockchip's `galcore.ko`. NXP's open galcore 6.4.11.p4 does not know it either, and the matching closed Vivante libraries exist only as Rockchip's 6.4.6, see `rnd/verisilicon/README.md`. If it is: the NPU die needs its own mainline device tree (RK1808 is not in mainline), a USB gadget and a server for the tensors, because the NPU sits behind USB and the host cannot drive it directly |
| Own kernel for the NPU die | a newer kernel is not possible (no RK1808 in mainline, `galcore` is tied to 4.4.185). A rebuild of the same 4.4.185 from Rockchip's `develop-4.4` is: Rockchip's `galcore.ko` (licence `Dual MIT/GPL`, `vermagic=4.4.185 SMP preempt mod_unload aarch64`, no `modversions`) loads into a kernel with that release string and compatible structures. Needs a reconstructed config (no `/proc/config.gz` on the NPU; the firmware kernel was built with Linaro GCC 6.3, build #27 of 2022-08-18); gain: own USB functions (network), trimming, boot time. Worth it only after the server works on the stock kernel. Also: ask Rockchip for the `galcore` sources (the licence says GPL) |
| Next LTS kernel | bump `sources.lock`, rebase `patches/` and `kernel-mods/`, [KERNEL.md](KERNEL.md) |
| ffmpeg with `mpp` (ffmpeg-rockchip), 1080p numbers | |
