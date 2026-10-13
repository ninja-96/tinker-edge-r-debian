# Recovery and backup

The RK3399 boot ROM cannot be erased. With no valid bootloader on any media it
enters **maskrom** and waits for USB download, so software mistakes are
recoverable.

## Before anything else

1. Back up the eMMC while the stock OS still runs:
   `sudo dd if=/dev/mmcblk0 bs=4M status=progress | gzip > emmc-backup.img.gz`
   Keep it: it may hold board-specific data (MAC address, serial).
2. Confirm you can enter maskrom and see the board: `rkdeveloptool ld`.
   The method (button or jumper) on Tinker Edge R still has to be documented
   here after checking the board.
3. Connect a 3.3 V USB-UART and keep logs. The ASUS U-Boot configuration
   (`tinker_edge_r_defconfig`) uses UART2 (0xFF1A0000) at **115200 8N1**.
   The stock kernel DTS puts its FIQ debugger on UART0 at 115200 instead, so
   if the kernel console stays silent while U-Boot prints, try UART0.

## Rules

- Test images on an SD card; do not write the eMMC until 24 h stress passes.
- Take regulator voltages, OPP tables and thermal zones from the stock DTS.
- Remove the SD card to return to whatever is on the eMMC.

## Flashing from Linux

SD card (works with any removable disk, with safety checks):

```sh
scripts/flash-sd.sh /dev/sdX
```

eMMC over USB (maskrom). Build the tools once:

```sh
./run.sh scripts/build-flash-tools.sh     # out/tools/rkdeveloptool + rk3399pro_loader.bin
sudo cp out/tools/99-rk-rockusb.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

Put the board in maskrom mode (method for the Tinker Edge R still to be
documented here: check `lsusb -d 2207:` shows a Rockchip device), connect the
USB-C data port to the host, then:

```sh
scripts/flash-emmc.sh --backup emmc-backup.img.gz        # --dry-run to preview
```

The script refuses without a backup file, a single Rockchip device in
maskrom/loader mode, and a typed `ERASE-EMMC` confirmation. It runs
`rkdeveloptool ld`, `db <loader>`, `wl 0 <image>`, `rd`.

## Restore the stock system

Flash the stock ASUS image the same way:
`scripts/flash-emmc.sh --skip-backup-check path/to/Tinker_Edge_R-...img`
(unzip the ASUS archive first). Untested on hardware.
