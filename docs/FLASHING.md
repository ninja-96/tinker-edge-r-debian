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

`scripts/flash-sd.sh /dev/sdX` writes the image with safety checks (removable disks
only, asks for confirmation, verifies). Whether the board boots from the card was
not tested: the BootROM tries the eMMC before the SD card, so a valid loader on the
eMMC wins.

## UMS mode (eMMC as a USB disk)

ASUS describes a mode in which their U-Boot exports the eMMC as a USB mass storage
device when a USB-C cable to a PC is connected at power-on (no jumper). It never
appeared on this board during the tests (the device "USB-MSC" seen on the bus is
the maskrom loader, not UMS), so the maskrom route is the documented one.
