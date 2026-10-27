# Links and sources

Everything this project used or learned from. The repository itself does not
depend on any of these at build time except the open upstream sources listed under
"Fetched by the build".

## ASUS

| What | Link |
|---|---|
| ASUS Tinker Edge R GitHub organisation (Debian kernel 4.4, U-Boot, ALPR demo) | https://github.com/TinkerEdgeR |
| Kernel 4.4 with the board device tree `rk3399pro-tinker_edge_r.dts` | https://github.com/TinkerEdgeR/debian-kernel |
| U-Boot 2017.09 of the stock image | https://github.com/TinkerEdgeR/debian-u-boot |
| Tinker Edge R wiki (GPIO table, console pins 8/10, flashing) | https://github.com/TinkerBoard/TinkerBoard/wiki/Tinker-Edge-R |
| Quick Start Guide (Recovery header J3, 12-19 V power) | https://tinker-board.asus.com/download/E16030_TINKER_EDGE_R_QSG_WEB.pdf |
| Schematics (block diagram: 2 x 2 GB LPDDR4, 2 GB LPDDR3 for the NPU, UART2 on the microSD socket) | https://tinker-board.asus.com/download/Tinker_Edge_R_Schematics.pdf |
| "Flash image through UMS mode" | https://github.com/TinkerBoard/TinkerBoard/files/12801859/TinkerEdgeR-Flash.Image-UMS.mode.pdf |
| Images and downloads | https://tinker-board.asus.com/download-list.html?product=tinker-edge-r |

## Rockchip

| What | Link |
|---|---|
| `rkbin`: DRAM init blob, BL31 and USB loader blobs (pinned: `3e288fe814e059dd06833495f845cab04ac20a5c`) | https://github.com/rockchip-linux/rkbin |
| `rkdeveloptool`: USB flashing tool (pinned: `304f073752fd25c854e1bcf05d8e7f925b1f4e14`) | https://github.com/rockchip-linux/rkdeveloptool |
| BSP kernel (branch `develop-6.12`: donor of the `mpp_service` port) | https://github.com/rockchip-linux/kernel |
| BSP U-Boot | https://github.com/rockchip-linux/u-boot |
| `mpp`: userspace media library (uses `/dev/mpp_service`) | https://github.com/rockchip-linux/mpp |
| NPU of RK1808/RV1126 (not for RK3399Pro) | https://github.com/rockchip-linux/rknpu , mirror https://github.com/airockchip/rknpu |
| NPU of RK3399Pro: firmware, `npu_transfer_proxy`, `librknn_api` | https://github.com/airockchip/RK3399Pro_npu |
| RKNN toolkit (model conversion on the PC) | https://github.com/rockchip-linux/rknn-toolkit |
| RKNN toolkit 2 / runtime for newer chips | https://github.com/rockchip-linux/rknn-toolkit2 , https://github.com/rockchip-linux/rknpu2 |

## Fetched by the build (open upstream sources)

| What | Link | Pinned in |
|---|---|---|
| Linux 6.18.56 | https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.56.tar.xz | `sources.lock` (sha256) |
| U-Boot v2026.10 | https://github.com/u-boot/u-boot | `sources.lock` |
| TF-A v2.15.0 (only for the open BL31) | https://github.com/ARM-software/arm-trusted-firmware | `sources.lock` |
| Debian 13 "trixie" packages | https://deb.debian.org/debian | `scripts/build-rootfs.sh` |

## Other references used in the research

| What | Link |
|---|---|
| coreboot open DRAM init for RK3399 | https://github.com/coreboot/coreboot/blob/main/src/soc/rockchip/rk3399/sdram.c |
| Etnaviv: Vivante GPU reverse engineering, `galcore` headers, `libvivhook` | https://github.com/etnaviv , https://github.com/laanwj/etna_viv |
| NXP Vivante kernel driver (same code family as the NPU's `galcore`) | https://github.com/nxp-imx/linux-imx (`drivers/mxc/gpu-viv`) |
| arm64 boot protocol | https://docs.kernel.org/arch/arm64/booting.html |
| RK3399Pro mainline device tree (Rock Pi N10) | https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/rockchip/rk3399pro-rock-pi-n10.dts |
| TIM-VX (VeriSilicon, the NPU graph library of the open stack) | https://github.com/VeriSilicon/TIM-VX |
| TFLite VX delegate | https://github.com/VeriSilicon/tflite-vx-delegate |
