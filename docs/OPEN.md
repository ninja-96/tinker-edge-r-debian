# Open source policy

**Rule:** the repository contains open source only. A closed component is allowed
in exactly two ways:

1. **Published unmodified** in `blobs/`, if its licence allows redistribution, with
   the licence text, the pinned origin and a checksum (`blobs/README.md`).
2. **Kept locally** (git-ignored) if it is not redistributable, and tracked here
   until it is replaced.

Closed components are replaced by open ones whenever that is possible.

## Closed parts of the image that boots on the board

| Component | Where | Status | Replacement |
|---|---|---|---|
| DRAM init blob `rk3399pro_ddr_933MHz_v1.30.bin` (the 800 MHz variant is kept as a fallback) | `blobs/rockchip/` | published (Rockchip licence) | none yet. U-Boot's open LPDDR4 TPL hangs on this board with the Rock Pi 4 timings; needs the real timings of the Edge R memory (2 x 2 GB LPDDR4) |
| BL31 `rk3399pro_bl31_v1.35.elf` | `blobs/rockchip/` | published (Rockchip licence) | open TF-A builds with `scripts/build-tfa.sh` (patch in `patches/tf-a/`); **not yet tested on the board**: `USE_TFA=1 ./run.sh scripts/build-uboot.sh` |
| Wi-Fi/BT firmware (`firmware-realtek`, `firmware-brcm80211`) | Debian `non-free-firmware`, installed at build time | not stored here | `FREE=1` builds a rootfs from Debian `main` only (no Wi-Fi) |
| NPU firmware of Rockchip: Linux 4.4.185 with the `galcore` driver, the Vivante user libraries (SDK 6.4.6), `MiniLoaderAll.bin`, `uboot.img`, `trust.img` | `airockchip/RK3399Pro_npu` (Apache-2.0, binary only) | fetched and sha256-checked by `scripts/fetch-npu.sh`, not stored here; installed as it is except the device tree and the ramdisk additions | none yet (see docs/NPU-RESEARCH.md). **Everything above the driver is open since 0.4**: TIM-VX, VeriSilicon's TFLite delegate, TensorFlow Lite with XNNPACK, our server, the USB channel, `npu-daemon`, the C++ library and its Python module; RKNN (`librknn_api`, `npu_transfer_proxy`, the RKNN-Toolkit) is not used any more |

Licences and sources of everything that is shipped are listed in [../NOTICE.md](../NOTICE.md). One point
that depends on how the Rockchip licence is read: the USB loader and the image loader repack the unmodified
Rockchip DRAM blob into other containers ([../blobs/README.md](../blobs/README.md)).

## Out of scope: the silicon

The BootROM and the firmware of the microcontrollers inside the SoC (power
management, NPU die) are fixed in hardware. They cannot be replaced and are not
counted as "closed parts to be replaced": the goal is that **everything we can
choose and load onto the board is open**.

## Local only, never published

The ASUS TinkerOS image and everything taken from it: stock bootloader pieces,
`upgrade_tool`, the eMMC backup. The scripts that read them
(`scripts/extract-stock-boot.sh`) are open; the files are not.

## Not supported yet: DisplayPort over USB-C

Mainline `cdn-dp` (the DP controller of the RK3399) only works with an `extcon` device
that reports `EXTCON_DISP_DP`. The vendor 4.4 kernel gets it from its own `fusb30x`
driver; the mainline FUSB302 driver does not provide one. `cdn_dp` is therefore disabled
in `dts/rk3399pro-tinker-edge-r.dts`, because an enabled `cdn-dp` that fails to probe
stops `rockchip-drm` from binding and takes HDMI down with it. Options: port the vendor
extcon part (GPL, from `github.com/TinkerEdgeR/debian-kernel`) or wait for an upstream
Type-C DP series. The same missing piece is why USB-C works only in host mode and
SuperSpeed only in one plug orientation.

## Next steps toward fully open

1. Test the open TF-A BL31 on the board.
2. Get working LPDDR4 timings for the open TPL (decode the parameters of the
   Rockchip blob with `rkbin/tools/ddrbin_tool.py`, or train by trial).
3. Open replacement for `upgrade_tool` (NPU firmware loader).
