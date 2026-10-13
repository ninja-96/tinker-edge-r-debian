# Tinker Edge R: modern Debian 13 + Linux LTS

Build scripts and patches for a current Debian 13 (arm64) image on the
**ASUS Tinker Edge R** (Rockchip RK3399Pro): Linux 6.18 LTS, hardware video
codecs, NPU.

> **Status: work in progress.** Nothing here has been booted on a board yet.
> See [docs/PLAN.md](docs/PLAN.md) for what works and what is planned.

## Goals

| Feature | Approach | Expected |
|---|---|---|
| Boot Debian 13 on Linux 6.18 LTS | mainline U-Boot + kernel + our DTS | yes |
| HW video decode | mainline `rkvdec` / `hantro` (V4L2) | yes |
| HW video encode (H.264, JPEG) | `mpp_service` ported from Rockchip BSP 6.12 | likely |
| NPU (RKNN 1.7.x) | firmware over USB + `npu_transfer_proxy`; small power-sequencing driver | likely |
| Audio: HDMI, then 3.5 mm jack (RK809) | mainline codec + `simple-audio-card` | likely |
| H.265 encode | not in the RK3399 hardware | no |
| Cameras | out of scope | no |

The NPU is a separate chip with its own frozen Linux 4.4 firmware; the host
kernel only provides the transport (PCIe/USB). Details in [docs/PLAN.md](docs/PLAN.md).

## Layout

| Path | Contents |
|---|---|
| `sources.lock` | pinned upstream versions (kernel, U-Boot, rkbin, mpp, NPU) |
| `scripts/` | `fetch-sources.sh`, `setup-binfmt.sh`, image build scripts |
| `docker/` | build environment (Debian 13, aarch64 cross toolchain) |
| `patches/linux/`, `patches/u-boot/` | our patches against upstream |
| `dts/` | Tinker Edge R device trees: base, `-mpp`, `-npu`, `-full` |
| `kernel-mods/` | new kernel sources: `mpp_service` port, NPU power driver |
| `overlay/` | files copied into the rootfs (NPU services, helper scripts) |
| `config/` | kernel config, `extlinux.conf`, rootfs settings |
| `docs/` | plan, recovery guide, notes |
| `run.sh` | run any command inside the build container |
| `downloads/`, `out/`, `build/` | generated, git-ignored |

## Quick start

Requires Docker (your user in the `docker` group) and your own copy of the
ASUS TinkerOS image (the stock bootloader and `upgrade_tool` are taken from it,
not redistributed).

```sh
./scripts/fetch-sources.sh                       # pinned upstream sources -> downloads/
./scripts/setup-binfmt.sh                        # arm64 emulation for the rootfs build (once per boot)
./run.sh scripts/build-rootfs.sh                 # Debian 13 rootfs      -> out/rootfs.tar
./run.sh scripts/build-kernel.sh                 # Linux 6.18 + patches  -> out/Image, out/*.dtb, out/modules
./scripts/extract-stock-boot.sh Tinker_Edge_R-Debian-Buster-v2.0.5-20220217.zip
./run.sh scripts/build-image.sh                  # SD image              -> out/tinker-edge-r-debian13.img
./run.sh scripts/qemu-smoke.sh                   # optional: userspace smoke test in QEMU
```

Write the image to an SD card (check the device name first!):

```sh
sudo dd if=out/tinker-edge-r-debian13.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Default login on the first boot: `tinker` / `tinker` (the password is expired
and must be changed at first login). Serial console: UART2, 115200 8N1.

## Device tree variants

The image boots the safe `base` tree. All variants are in `/boot/dtbs`; switch
on the board with `sudo tinker-boot-select <variant>` and reboot (the previous
boot image is kept in `/var/lib/tinker/boot.prev`).

| Variant | Adds |
|---|---|
| `base` | mainline drivers: Ethernet, USB, HDMI, GPU, V4L2 video decode |
| `mpp` | Rockchip `mpp_service` instead of V4L2 video (`/dev/mpp_service`, H.264 encode) |
| `npu` | NPU power sequencing; firmware load and `npu_transfer_proxy` services |
| `full` | `mpp` + `npu` |

An alternative boot path with mainline U-Boot and extlinux (untested fallback,
for when the stock bootloader refuses the mainline kernel):

```sh
./run.sh scripts/build-uboot.sh
./run.sh scripts/build-image-mainline-uboot.sh
```

## Safety

Work from an SD card and keep the eMMC untouched until an image is proven.
Back up the eMMC first. The RK3399 boot ROM allows recovery over USB (maskrom),
see [docs/RECOVERY.md](docs/RECOVERY.md). A 3.3 V USB-UART adapter is strongly
recommended: the stock U-Boot console is UART2 at 115200 8N1 (see the
recovery guide). First-boot checklist: [docs/BRINGUP.md](docs/BRINGUP.md).

## Licenses

Scripts and docs: MIT. Kernel patches, drivers and device trees: GPL-2.0
(device trees GPL-2.0+ OR MIT). Third-party components keep their own licenses;
see [NOTICE.md](NOTICE.md). The stock ASUS image and its binaries are **not**
distributed here.
