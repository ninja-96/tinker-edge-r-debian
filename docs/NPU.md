# RK3399Pro NPU on the Tinker Edge R

> **Status: works on the board (verified 2026-10-26), part of the 0.3 image.** The NPU stack is
> closed (see [OPEN.md](OPEN.md)). Its files come from Rockchip's Apache-2.0 repository
> `airockchip/RK3399Pro_npu` at a pinned commit, downloaded and sha256-checked by
> `scripts/fetch-npu.sh` and installed unchanged, except for the device tree inside the NPU boot image
> (below). Verified with exactly these files: power-up, firmware load without any closed tool,
> RKNN transport, MobileNet through `librknn_api` and through the Python module
> (74 inferences/s, 3000 in a row without errors).

The NPU is a separate die with its own CPU, DDR and a frozen Linux 4.4.185.
The host only powers it, loads its firmware and talks to it through the
`npu_transfer_proxy` daemon and `librknn_api`.

## Transport: USB

The stock device tree has no `rockchip,deferred` property, so the vendor
kernel never creates `pcie_reset_ep`, and the ASUS `npu-image.sh` takes the
USB branch (`npu_upgrade` + `/usr/share/npu_fw`). No PCIe patch is needed; the
BSP's `rockchip-pcie-dma` and deferred-link code are not used on this board.
(The external PCIe root complex carries the Wi-Fi module.)

Confirm on the stock OS with `npu_transfer_proxy devices`: it should report a
`USB_DEVICE`.

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
  ADB and an RKNN (NTB) interface.

## Userspace

| File | Origin |
|---|---|
| `npu_transfer_proxy` (2.1.0), `librknn_api.so` and `rknn_api.h` (1.7.5), firmware `MiniLoaderAll.bin`, `uboot.img`, `trust.img`, `boot.img` | https://github.com/airockchip/RK3399Pro_npu, commit in `sources.lock`, fetched by `scripts/fetch-npu.sh` (Apache-2.0, binary only; the GPL parts inside the firmware, Linux 4.4.185, BusyBox and the galcore driver, have their sources at Rockchip) |
| `rkdeveloptool` with the `rs` command | `third_party/rkdeveloptool` plus `patches/rkdeveloptool/` (GPL-2.0+), built for arm64 by `scripts/build-rkdeveloptool-arm64.sh` |
| `npu-load`, `npu-proxy-run`, systemd units | this repository, `overlay/` |
| `rk3399pro_npu` (Python module) | this repository, `python/` (MIT) |

**Loading the firmware needs no closed tool.** The loader sequence of the stock `upgrade_tool`
(`db`, `td`, `wl` x3, `rs`) is done by `rkdeveloptool`; the missing `rs` ("run system") was added after
watching its USB traffic, see `rnd/npu/RS-PROTOCOL.md` in the working copy and the patch.

**One change to Rockchip's firmware.** The image is made for Rockchip's evaluation board, where the NPU
is powered by two I2C regulators (`tcs452x`, `syr837`). On the Tinker Edge R the kernel of the image
fails with `galcore: failed to get regulator` and the NPU resets the moment a model is loaded. The
ASUS image has a `fan53555` there, which the Rockchip kernel's driver does not accept ("Chip ID 0 with rev 0 not
supported"). `scripts/npu-fw-tinker.py` therefore replaces the two regulators in the device tree inside
`boot.img` with one fixed regulator (0.85 V, no control over I2C, so the NPU stays at the voltage that
the board sets by default); kernel and root file system are copied as they are. With ASUS images
the script is not needed.

The stock `npu_transfer_proxy` reads the `Hardware` and `Serial` lines of `/proc/cpuinfo`,
which only the vendor kernel prints, and otherwise exits silently with status 0.
`/usr/local/sbin/npu-proxy-run` therefore starts it in its own mount namespace with a
private `/proc/cpuinfo` that has those two lines (the serial is the NPU's USB serial).

Services (active only with an `-npu` or `-full` DTB, via
`ConditionPathExists=/proc/device-tree/npu-power`):

- `npu-firmware.service`: runs `npu-load` (reset, `rkdeveloptool db/td/wl/rs`)
- `npu-transfer-proxy.service`: runs the proxy

Enable on the board: `sudo tinker-boot-select npu` (or `full`), reboot.

## ADB on the NPU die

The stock NPU firmware enables ADB next to the RKNN link (`/etc/init.d/.usb_config` has
`usb_adb_en` and `usb_ntb_en`). With the NPU running: `sudo apt install adb`,
`sudo adb shell` gives a root shell in the NPU's own Linux 4.4 (Buildroot, 2 GB RAM).

## Known problems

- If a client is killed while talking to the NPU, `rknn_server` on the NPU can stay stuck and
  the next `rknn_init` hangs. Restart the NPU side: stop `npu-transfer-proxy`, restart
  `npu-firmware`, start `npu-transfer-proxy`.
- Tested with the ASUS copy of the firmware and proxy (proxy 2.0.0, `rknn_server` 1.6.0) and
  `librknn_api` 1.7.5.
