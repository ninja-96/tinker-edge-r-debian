# Flashing

Tools: `scripts/backup-emmc.sh`, `scripts/flash-emmc.sh`, `scripts/verify-emmc.sh`,
`scripts/flash-at.sh`, `scripts/flash-sd.sh`; built by `scripts/build-flash-tools.sh`
into `out/tools/` (`rkdeveloptool` from `third_party/`, USB loader from
`blobs/rockchip/`).

## Entering maskrom (Quick Start Guide of the board)

1. Power off, USB-C data cable between the board's USB-C port and the PC.
2. Short the **Recovery header J3** with a jumper.
3. Connect the 12-19 V power adapter.
4. `lsusb -d 2207:` must list "RK3399 in Mask ROM mode"; then **remove the jumper**
   (the board stays in maskrom until power is removed; with J3 shorted the eMMC may
   not be accessible).

Use a USB 2.0 port directly on the PC. After flashing, remove J3 and the USB cable
and power-cycle: with J3 shorted the board enters maskrom again at every start.

## Backup and write

```sh
scripts/backup-emmc.sh emmc-backup.img          # reads the whole eMMC, writes .sha256
scripts/flash-emmc.sh --backup emmc-backup.img  # writes out/tinker-edge-r-debian13.img
```

`flash-emmc.sh` refuses to run without a backup file, writes the image in 16 MiB
pieces with retries, reads the eMMC back and compares everything, and only then
resets the board. Resume after a failure with `--start-mb N`.

Restore the original system: `scripts/flash-emmc.sh --skip-backup-check emmc-backup.img`.

Quick rewrites of a part (loader, boot image) without a full flash:
`scripts/flash-at.sh <LBA> <file>`, e.g. `scripts/flash-at.sh 64 out/u-boot-rockchip-ddrblob.bin`.

## Pitfalls found the hard way

| Symptom | Cause | What to do |
|---|---|---|
| Write reports success but the eMMC keeps the old data after 32 MiB | the **RK3399Pro** USB loader silently drops writes beyond 32 MiB | use the **RK3399** loader (`rk3399_loader.bin`, the default); `scripts/emmc-write-test.sh <loader>` checks a loader |
| `rkdeveloptool ld` says "Maskrom" after the loader was downloaded | `ld` keeps printing the BootROM state | ask the board instead: `rkdeveloptool rfi` answers only when the loader runs (`scripts/rockchip-usb.sh`) |
| Second `db` hangs (exit 124) | the loader is already running | skip `db` if `rfi` answers (done by the scripts) |
| `Creating Comm Object failed` | no permission on the USB device, or another process holds it | run through `sudo`, or install the udev rule `third_party/rkdeveloptool/99-rk-rockusb.rules`; close other users of the device |
| Mismatch only in MiB 3 of the image | the board rewrites its own area (vendor storage around sector 7168) | ignored by the verification |
| Mismatch only in the last MiB | the image does not end on a MiB boundary | the comparison handles the shorter last piece |
| Board silent after flashing | wrong console pins/baud, or the image did not boot | see [UART.md](UART.md), [BRINGUP.md](BRINGUP.md) |

## microSD

Build the SD variant from the existing build artifacts:

```sh
SD=1 ./run.sh scripts/build-image-mainline-uboot.sh
scripts/flash-sd.sh --dry-run /dev/sdX
scripts/flash-sd.sh /dev/sdX
```

The SD image contains the OS and its own U-Boot. The outputs are
`out/tinker-edge-r-debian13-sd.img` and the 16 MiB
`out/tinker-edge-r-uboot-emmc.img`. The script checks that the disk
is removable or USB, asks for confirmation and verifies the write. The SD variant
has a different root partition UUID and label from the eMMC image, so they can
coexist without ambiguous root filesystem selection.

The U-Boot control DT uses **mmc1 for SD and mmc0 for eMMC**, unlike Linux.
The default order is `mmc1 mmc0 ...`; v0.7 checks it at build time. An existing
saved environment can override it. Fallback does not recover from a Linux hang
after the kernel has started. Insert the card while powered off, then power on.
If it does not boot from the card, connect UART0 (see [UART.md](UART.md)), interrupt
autoboot, then run these U-Boot commands without saving the environment:

```text
mmc list
setenv boot_targets mmc1 mmc0 nvme scsi usb pxe dhcp spi
bootflow scan -lb
```

Verify `findmnt -no SOURCE /` shows `/dev/mmcblk0p1`, the root filesystem has grown
and `swapon --show` lists the 2 GiB swapfile. The filesystem expands to the full
card capacity with no root-only reserved blocks in v0.7. Run `scripts/test-board.sh`.
If a full eMMC OS is installed, removing the card should restore eMMC boot.

### Standalone SD (no eMMC loader)

An SD card with this image also boots when the eMMC has no valid loader or
partition table (warm reboot and cold power cycle verified on the Tinker Edge R
on 2026-11-27). On a board with
already empty eMMC, simply write the SD image, insert the card and power on.
There is no need to flash the loader-only eMMC artifact in this configuration.
Do not erase an existing eMMC installation just to try SD; back it up first.

### Loader-only eMMC and SD operating system

This optional recovery configuration uses a locally built 16 MiB loader-only image
`tinker-edge-r-uboot-emmc.img` (`SD=1 scripts/make-image.sh`). Only the complete SD
image is published. To derive the loader-only image from a verified, decompressed
release SD image in `out/`, without rebuilding:

```sh
dd if=/dev/zero of=out/tinker-edge-r-uboot-emmc.img bs=1M count=16
dd if=out/tinker-edge-r-debian13-sd.img of=out/tinker-edge-r-uboot-emmc.img bs=512 skip=64 seek=64 count=32704 conv=notrunc
./run.sh scripts/build-flash-tools.sh
```

With the card removed, enter maskrom as described above, then:

```sh
scripts/backup-emmc.sh emmc-backup.img
scripts/flash-emmc-loader.sh --backup emmc-backup.img
```

The second command writes and verifies the loader at LBA 64, then removes the
primary and backup partition tables. It requires a full-size backup and explicit
`LOADER-ONLY` confirmation. No eMMC OS will remain bootable; this is not a secure
erase of old data. Keep the backup for recovery. Write the SD image with
`scripts/flash-sd.sh`, disconnect USB-C, remove J3, insert the card with power off,
then power on. An SD card is now required to boot Debian. Subsequent OS releases
only require rewriting the card, not the eMMC loader. This dedicated MaskROM
flashing script has been tested offline for write ranges and safety checks;
its loader-only hardware installation is still to be verified separately.

### One-time migration from v0.6 (mainline U-Boot on eMMC)

If you want to retain the existing eMMC Debian installation as fallback, you do not
need to reflash it. The v0.6 default is already SD-first; if the saved environment
has a different order, with the SD card **removed**,
interrupt autoboot over UART0, check `mmc list` (SD = mmc1, eMMC = mmc0) and
`printenv boot_targets`, then set and save the new order:

```text
setenv boot_targets mmc1 mmc0 nvme scsi usb pxe dhcp spi
saveenv
reset
```

The environment is stored on the MMC device from which SPL booted; removing the
card ensures it is saved to eMMC on a v0.6 installation. To revert, repeat with
`setenv boot_targets mmc0 mmc1 nvme scsi usb pxe dhcp spi` and `saveenv`.
Back up eMMC before modifying its boot environment (see above). Older or stock ASUS
bootloaders may have a different environment or layout: do not use these commands
as a migration procedure for an unverified bootloader. For a board with an existing
Debian installation, retain it as fallback if desired. On a fresh board, install
the loader-only image as described above. For an original ASUS installation or an unknown bootloader,
the supported migration route is to back up the original eMMC and install the
project's eMMC image once (build it with `scripts/make-image.sh`, or use the
published v0.6 image followed by the migration above). This establishes a known
Debian fallback instead of assuming a new U-Boot can boot the stock ASUS layout.
Subsequent releases from v0.7 only require rewriting the SD card.

## UMS mode (eMMC as a USB disk)

ASUS describes a mode in which their U-Boot exports the eMMC as a USB mass storage
device when a USB-C cable to a PC is connected at power-on (no jumper). It never
appeared on this board during the tests (the device "USB-MSC" seen on the bus is
the maskrom loader, not UMS), so the maskrom route is the documented one.
