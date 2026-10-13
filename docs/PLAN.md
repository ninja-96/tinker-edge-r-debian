# Plan

Principle: stay on the proven stock bootloader first, change one layer at a time.
Every stage ends with an acceptance check on the board.

| # | Stage | Work | Acceptance |
|---|---|---|---|
| 0 | Environment | Docker builder, pinned sources, repo skeleton, Debian 13 rootfs | builder image builds, sources fetch, `out/rootfs.tar` |
| 1 | Mainline boot | Tinker Edge R DTS on mainline 6.18, kernel config, boot image for the stock U-Boot (Android boot.img), SD image | boots from SD; serial console, SSH, Ethernet, HDMI, USB |
| 2 | HW video | port `mpp_service` (vepu2, vdpu2, rkvdec, jpgenc) from Rockchip BSP 6.12; decode via mainline `rkvdec`/`hantro` | H.264 1080p encoded in hardware |
| 3 | NPU | power driver (done, untested), firmware load over USB via `upgrade_tool`, `npu_transfer_proxy`, RKNN demo | RKNN demo runs |
| 3a | Audio | HDMI first, then 3.5 mm jack (RK809 codec) | `aplay` on both outputs |
| 4 | Hardening | Wi-Fi/BT, thermal/DVFS, 24 h stress, mainline U-Boot + extlinux, eMMC install, docs | stable under load |

## Status (built, nothing run on a board yet)

| Item | State |
|---|---|
| Debian 13.7 rootfs, Linux 6.18.56 + 4 DTB variants, SD image | built, structure verified |
| Userspace smoke test (QEMU virt) | passes: systemd, ssh, networkd, modules, helper scripts |
| `mpp_service` port (RKVDEC, VDPU2, VEPU2) | compiles as module; untested |
| NPU power driver + services | compiles; untested (needs `npu`/`full` DTB) |
| Mainline U-Boot fallback image | built; DDR assumed LPDDR3 4 GB; untested |
| Hardware checks | pending: see docs/BRINGUP.md |

## Fallback

The stock ASUS system stays the known-good reference: remove the SD card and
the board boots it again. Rockchip BSP 6.12 is used only as a patch donor; its
RK3399Pro device trees are not buildable (`rk3399pro.dtsi` is missing).

## Known facts

- NPU firmware is loaded into NPU RAM at every boot, so it cannot be bricked.
- NPU transport is USB on this board (see docs/NPU.md); confirm with
  `npu_transfer_proxy devices` on the stock OS.
- The RK3399 VPU has no H.265 encoder.
- Stock kernel is 4.4.194; the NPU runs its own 4.4.185 and cannot be updated.
