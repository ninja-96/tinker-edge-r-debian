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

## Restore

TODO: maskrom flashing procedure with `rkdeveloptool` (to be verified on the board).
