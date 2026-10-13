# RK3399Pro NPU on the Tinker Edge R

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
`/sys/bus/platform/drivers/tinker-npu-power/*/{power,reset}`.

## Userspace

| File | Source |
|---|---|
| `/usr/bin/npu_transfer_proxy`, `/usr/share/npu_fw/*`, `librknn_api.so` | `airockchip/RK3399Pro_npu` (Apache-2.0), or your ASUS image |
| `/usr/bin/upgrade_tool` | **not redistributable**: extracted from your ASUS image |
| `npu-load`, `npu_powerctrl` shim, systemd units | `overlay/` |

Without `upgrade_tool` the firmware cannot be loaded (`rkdeveloptool` lacks the
`td` and `rs` commands used by the sequence).

Services (active only with an `-npu` or `-full` DTB, via
`ConditionPathExists=/proc/device-tree/npu-power`):

- `npu-firmware.service`: runs `npu-load` (reset, `upgrade_tool db/td/wl/rs`)
- `npu-transfer-proxy.service`: runs the proxy

Enable on the board: `sudo tinker-boot-select npu` (or `full`), reboot.

## Not verified

None of this has run on hardware yet.
