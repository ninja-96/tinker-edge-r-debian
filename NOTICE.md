# Third-party components, licences and sources

This repository holds build scripts, patches, device trees, small kernel and U-Boot additions,
a few unmodified third-party files and documentation. Everything else is downloaded by the
build scripts (`fetch-sources.sh`, `build-mpp.sh`, `build-gst-rockchip.sh`, the Debian archive)
and keeps its own licence. The licence texts used here are in [`LICENSES/`](LICENSES/).
This is the project's own summary, not legal advice.

## 1. Files in this repository

| Path | Licence | Origin |
|---|---|---|
| `scripts/`, `docker/`, `docs/`, `overlay/`, `run.sh`, `sources.lock`, `README.md`, `NOTICE.md` | MIT (`LICENSE`, `LICENSES/MIT.txt`) | own work |
| `dts/rk3399pro-tinker-edge-r.dts` | GPL-2.0+ OR MIT | **port of the vendor device tree** `rk3399pro-tinker_edge_r.dts` (Copyright (c) 2018 Fuzhou Rockchip Electronics Co., Ltd, `GPL-2.0+ OR MIT`, from `TinkerEdgeR/debian-kernel`); voltages, GPIOs and bus layout are taken from it, the rest is new. Same licence as the source |
| `dts/*-mpp.dtsi`, `dts/*-npu.dtsi`, wrapper `.dts` files | GPL-2.0+ OR MIT | from the Rockchip BSP 6.12 `rk3399.dtsi` (mpp nodes) and own work |
| `kernel-mods/drivers/soc/rockchip/mpp/` (23 files) | GPL-2.0+ OR MIT | port of the Rockchip BSP 6.12 `mpp_service`; **licence tags and Rockchip copyright lines are those of the originals**. `mpp_compat.h` is ours (GPL-2.0) |
| `kernel-mods/.../tinker-npu-power.c` | GPL-2.0 | own work |
| `kernel-mods/include/uapi/linux/rk-mpp.h` | (GPL-2.0+ WITH Linux-syscall-note) OR MIT | Rockchip BSP 6.12 |
| `patches/linux/*.patch` | GPL-2.0 | patches against the Linux kernel; `0001` exports the IOMMU helpers that the Rockchip BSP kernel (GPL-2.0) also has, `0002` adds one chip revision to `fan53555` |
| `config/` | GPL-2.0 | kernel configuration fragment |
| `u-boot-mods/` | GPL-2.0+ | additions to U-Boot (board defconfig, device tree include) |
| `patches/tf-a/*.patch` | BSD-3-Clause | patch against Trusted Firmware-A |
| `third_party/rkdeveloptool/` | GPL-2.0+ (sources), `license.txt` is the GPL-2 text | Rockchip's flashing tool, **byte-identical** to `rockchip-linux/rkdeveloptool` commit `304f0737`, see its `ORIGIN.md` |
| `blobs/rockchip/` | Rockchip licence, see below | unmodified Rockchip binaries and one file assembled from them |

## 2. Binary-only files (Rockchip)

Origin: `rockchip-linux/rkbin` at commit `3e288fe814e059dd06833495f845cab04ac20a5c`
(details and checksums: [`blobs/README.md`](blobs/README.md)).

| File | What it is |
|---|---|
| `rk3399pro_ddr_933MHz_v1.30.bin` | DRAM initialisation at 933 MHz (the default of the image) |
| `rk3399pro_ddr_800MHz_v1.30.bin` | the same at 800 MHz (fallback) |
| `rk3399pro_bl31_v1.35.elf` | EL3 firmware (PSCI) |
| `rk3399_loader.bin` | USB loader for flashing over maskrom. **Assembled by us** with Rockchip's `tools/boot_merger` from three unmodified `rkbin` files (DDR, USB plug, miniloader); the payloads are not changed, but the container is new |

Licence (`blobs/rockchip/LICENSE`, Rockchip's text): "a non-exclusive license (a) to use, copy,
distribute the Software"; **not** allowed: decompiling, reverse engineering, removing or obscuring
copyright, patent or trademark notices. We do none of that. The licence says nothing about merging
binaries into one container (the loader, and the DRAM blob that `mkimage` packs into the image
loader), which is the one point a reader may want to judge for themselves.

## 3. Sources downloaded and built at build time (not stored here)

| Component | Licence | Used as |
|---|---|---|
| Linux kernel (kernel.org tarball, version and sha256 in `sources.lock`) | GPL-2.0 (with the syscall exception for UAPI headers) | patched and built by `scripts/build-kernel.sh` |
| U-Boot (tag in `sources.lock`) | GPL-2.0+ | built by `scripts/build-uboot.sh` |
| Trusted Firmware-A (tag in `sources.lock`), only with `USE_TFA=1` | BSD-3-Clause | built by `scripts/build-tfa.sh` |
| Rockchip `mpp` (commit in `sources.lock`) | Apache-2.0 OR MIT for almost all files; `osal/allocator/ion.h` is a copy of a Linux kernel header under GPL-2.0 | userspace codec library, built by `scripts/build-mpp.sh` |
| Rockchip GStreamer plugins (`JeffyCN/mirrors`, branch `gstreamer-rockchip`, commit in `sources.lock`) | LGPL-2.0-or-later (file headers; `COPYING` is the LGPL-2.1 text) | built by `scripts/build-gst-rockchip.sh` |
| Debian 13 packages (kernel modules excepted), including the `non-free-firmware` packages | per package; see `/usr/share/doc/<package>/copyright` in the image | installed into the root filesystem |

Used for research only and **never distributed**: the ASUS TinkerOS image, ASUS schematics and
wiki copies (`reference/`, git-ignored), and the closed NPU vendor files used in local tests
(`third_party/npu/`, git-ignored; see [`docs/NPU.md`](docs/NPU.md)).

## 4. Sources for the release images

The images attached to the GitHub releases contain, besides Debian, the following that we build.
The complete corresponding source of the GPL and LGPL parts is available as follows and will stay
available for at least three years after the release (this is our written offer; if a link is
gone, write to the repository's issue tracker):

| In the image | Source |
|---|---|
| Linux kernel and its modules | the kernel.org tarball named in `sources.lock`, plus `patches/linux/`, `kernel-mods/`, `config/linux-tinker.fragment`, `dts/` of the release tag |
| U-Boot | the tag named in `sources.lock`, plus `u-boot-mods/` of the release tag |
| Rockchip `mpp`, GStreamer plugin | the repositories and commits named in `sources.lock` |
| Debian packages | the Debian source archive (`deb-src` of suite `trixie`, https://deb.debian.org/debian) and https://snapshot.debian.org for old versions; the exact package versions are listed in `/usr/share/doc/tinker-edge-r/packages.txt` in the image (and as `packages.txt` with the release, from release 0.3 on) |
| Rockchip binaries (DRAM init, BL31) | no source (see section 2) |

The licence texts and this notice are in the image under `/usr/share/doc/tinker-edge-r/` (from the
release after 0.2).

## 5. Names

"ASUS", "Tinker Edge R", "Rockchip" and "RK3399Pro" are names of their owners and are used here only to
say which hardware and which software this project is about. The project is not affiliated with or endorsed
by ASUS or Rockchip.
