# Third-party components

This repository contains only build scripts, patches, device trees and
documentation. Upstream sources are downloaded by `scripts/fetch-sources.sh`
and keep their own licenses.

| Component | License | How used |
|---|---|---|
| Linux kernel, our kernel patches and DTS | GPL-2.0 | downloaded, patched |
| U-Boot, our patches | GPL-2.0+ | downloaded, patched |
| Rockchip BSP kernel (`rockchip-linux/kernel`) | GPL-2.0 | patch source (`mpp_service`, PCIe NPU hooks) |
| Rockchip `mpp` | Apache-2.0 | userspace codec library |
| Rockchip `rkbin` (DDR init, BL31 blobs) | Rockchip license: use/copy/distribute allowed, no reverse engineering, keep notices | downloaded, never modified |
| `blobs/rockchip/` (DRAM init blob, BL31) | Rockchip licence (use/copy/redistribute, no modification or reverse engineering) | committed unmodified with its licence and origin, see blobs/README.md |
| `airockchip/RK3399Pro_npu` (NPU firmware, `npu_transfer_proxy`, `librknn_api`) | Apache-2.0 | downloaded |
| `kernel-mods/.../mpp/` (port of Rockchip BSP 6.12 `mpp_service`) | GPL-2.0+ OR MIT (as upstream); the port stays dual-licensed | copied into the kernel tree at build time |
| `kernel-mods/.../tinker-npu-power.c`, `patches/linux/*.patch` | GPL-2.0 | copied/applied to the kernel tree at build time |
| Debian packages | per-package (mostly DFSG-free) | rootfs |
| ASUS TinkerOS image | ASUS terms | **not distributed**; research only |

## Licenses of this repository's own files

| Path | License |
|---|---|
| `scripts/`, `docker/`, `docs/`, `overlay/`, `run.sh`, `sources.lock`, `README.md` | MIT (`LICENSE`, `LICENSES/MIT.txt`) |
| `dts/` | GPL-2.0+ OR MIT (SPDX headers in the files) |
| `u-boot-mods/`, `patches/tf-a/` | GPL-2.0+ (U-Boot) / BSD-3-Clause (TF-A patch) |
| `kernel-mods/`, `patches/linux/`, `config/` | GPL-2.0 (`LICENSES/GPL-2.0.txt`); these are parts of or patches to the Linux kernel; the mpp sources keep their upstream dual GPL-2.0+/MIT license |

Third-party blobs and sources are never stored here; see the table above for how they are obtained.
