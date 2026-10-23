# Boot chain

What runs from power-on to the login prompt on the image built here, and why it is
built this way.

```
BootROM (in the SoC, fixed)
  -> DRAM init blob   (Rockchip, blobs/rockchip/rk3399pro_ddr_800MHz_v1.30.bin)
  -> U-Boot SPL       (open, built from U-Boot v2026.10)
  -> BL31             (Rockchip blob; open TF-A builds, untested on the board)
  -> U-Boot proper    (open)
  -> extlinux         (/boot/extlinux/extlinux.conf on the rootfs partition)
  -> Linux 6.18       (open, our device tree)
```

## eMMC layout (written by `scripts/build-image-mainline-uboot.sh`)

| Where | Content |
|---|---|
| LBA 0 | GPT |
| LBA 64 | `idbloader`: the DRAM blob + SPL (`mkimage -T rksd`) |
| LBA 16384 | `u-boot.itb`: U-Boot proper + BL31 (FIT image) |
| LBA 32768 (16 MiB) | partition 1 `rootfs` (ext4), with `/boot/Image`, `/boot/dtbs/`, `/boot/extlinux/` |

Kernel command line (in `extlinux.conf` and in the device tree `chosen`):
`earlycon=uart8250,mmio32,0xff180000 console=tty1 console=ttyS0,115200n8
root=PARTUUID=... rw rootfstype=ext4 rootwait`.

## Why this and not the ASUS bootloader

The first attempts used the bootloader extracted from the stock TinkerOS image
(U-Boot 2017.09 loading an Android `boot.img`). With it the board never showed a
sign of life. What was established:

1. The ASUS U-Boot prints on UART2, which is on the microSD socket pads, so there was
   no output on the header (see [HARDWARE.md](HARDWARE.md)).
2. It runs a raw arm64 `Image` in place, at an address that is 0x80000 past a 2 MiB
   boundary (the `text_offset` convention of the 4.4 kernel). A mainline kernel
   has `text_offset = 0` and must start exactly on a 2 MiB boundary
   ([booting.rst](https://docs.kernel.org/arch/arm64/booting.html)). A 1.5 MiB
   "trampoline" in front of the kernel (`scripts/mkbootimg.py`) was added to
   realign it, but no sign of life followed, so the cause was never confirmed.
3. A mainline U-Boot with extlinux boots the same kernel, device tree and rootfs
   without problems. The kernel and the image were therefore fine; the failing part
   was the hand-over from the ASUS bootloader.

That path is kept as `scripts/build-image.sh` (legacy, never booted).

## DRAM initialisation

U-Boot has an open DRAM init (TPL, `sdram_rk3399.c`, same lineage as coreboot's
`src/soc/rockchip/rk3399/sdram.c`). With the LPDDR4 timings of the Rock Pi 4
(`rk3399-sdram-lpddr4-100.dtsi`) it stops silently right after the banner
`U-Boot TPL 2026.10`. On success it prints `Channel 0: LPDDR4 ...`.

The Rockchip DRAM blob trains the memory itself and works: `DRAM: 4 GiB`. It is
packed in front of the open SPL:

```
mkimage -n rk3399 -T rksd -d rk3399pro_ddr_800MHz_v1.30.bin:u-boot-spl.bin idbloader.img
```

Replacing the blob needs the real timings of the memory chips (see
[OPEN.md](OPEN.md)); the blob's parameters can be read with `rkbin/tools/ddrbin_tool.py`.

## BL31

The Rockchip BL31 (`rk3399pro_bl31_v1.35.elf`) is used. The open TF-A
(`PLAT=rk3399`) did not link with binutils 2.44: the PMU SRAM region overflowed by
3928 bytes because the long-call veneers of `pmu_cpuson_entrypoint` made the linker
add a page. `patches/tf-a/0001-...patch` replaces the two direct `bl` by
register-indirect calls; BL31 then fits (7 KiB of 8 KiB). Build it with
`scripts/build-tfa.sh` and use it with `USE_TFA=1 ./run.sh scripts/build-uboot.sh`.
It has not been run on the board yet.

## Console

Kernel and U-Boot (SPL and proper) print on UART0, header pins 8/10, 115200.
The device tree enables `uart0`; U-Boot gets `CONFIG_DEBUG_UART_BASE=0xFF180000`.
