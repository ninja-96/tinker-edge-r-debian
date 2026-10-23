# Binary-only components

This repository is open source. The few binaries below have no source available
to us; they are published **unmodified**, with their licence and origin, because
their licences allow redistribution. Replacing them with open code is tracked in
[docs/OPEN.md](../docs/OPEN.md).

| File | What it does | Origin (pinned) | Licence |
|---|---|---|---|
| `rockchip/rk3399pro_ddr_800MHz_v1.30.bin` | DRAM initialisation and training (runs first, in SRAM) | https://github.com/rockchip-linux/rkbin, commit `3e288fe814e059dd06833495f845cab04ac20a5c`, `bin/rk33/` | Rockchip, see `rockchip/LICENSE` |
| `rockchip/rk3399pro_bl31_v1.35.elf` | EL3 firmware (PSCI, power management) | same | same |
| `rockchip/rk3399_loader.bin` | USB loader for flashing the eMMC from the PC (maskrom mode); runs on the board | built with Rockchip's `tools/boot_merger` and `RKBOOT/RK3399MINIALL.ini` from the same `rkbin` commit, out of the unmodified `rk3399_ddr_800MHz_v1.30.bin`, `rk3399_usbplug_v1.30.bin`, `rk3399_miniloader_v1.30.bin` | same |

Checksums: `rockchip/SHA256SUMS` (`sha256sum -c SHA256SUMS`).

Inputs of `rk3399_loader.bin` (sha256, from `rkbin/bin/rk33/`):

```
2c74f7c0a2f7b1a49225fd0dca2fd88f574177e2b87dd4c526d569a94a9bcd92  rk3399_ddr_800MHz_v1.30.bin
ac4babf95a79822936236a7c0f8ef4db16582e109a1c5e23c7ded62509cfdff5  rk3399_usbplug_v1.30.bin
17b4dc35fc88f9c9648a5428c5b6453cdd6759f74e90e8dce25f0b22d2db1c03  rk3399_miniloader_v1.30.bin
```

(`rk3399_ddr_800MHz_v1.30.bin` and `rk3399pro_ddr_800MHz_v1.30.bin` are byte-identical.)
The RK3399 loader is the one to use: the `rk3399pro` USB loader silently drops eMMC
writes beyond 32 MiB on this board (see docs/FLASHING.md).

## Licence and how this repository complies

The files in `rockchip/` come from Rockchip's `rkbin` repository and are
distributed under its licence: [LICENSE at the pinned commit](https://github.com/rockchip-linux/rkbin/blob/3e288fe814e059dd06833495f845cab04ac20a5c/LICENSE)
(copy: `rockchip/LICENSE`). It grants "a non-exclusive license (a) to use, copy,
distribute the Software" and forbids decompiling, reverse engineering and removing
copyright, patent or trademark notices.

What we do, and nothing more:

- the files are copied **unmodified** (checksums in `rockchip/SHA256SUMS`);
- the licence text and the origin travel with them;
- they are used only as the DRAM initialisation stage and the EL3 firmware of the
  boot chain (the DRAM blob is packed into the bootloader image with U-Boot's
  `mkimage`, which only wraps it in the Rockchip image format);
- nobody decompiles or reverse engineers them here, and no notice is removed.

Binaries that are **not** redistributable (the ASUS TinkerOS image, its bootloader
pieces and tools) are never committed: they stay in git-ignored local directories.
