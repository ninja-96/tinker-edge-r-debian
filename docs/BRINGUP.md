# Bring-up checklist

Nothing in this repository has run on a board yet. Work through these in
order; stop at the first failure and keep the UART log.

## 0. Before touching anything

1. On the stock OS (eMMC), collect facts and a backup:
   ```sh
   cat /proc/device-tree/model
   npu_transfer_proxy devices          # expect USB_DEVICE (see docs/NPU.md)
   lsusb; lspci; lsblk
   dmesg | head -200 > stock-dmesg.txt
   sudo dd if=/dev/mmcblk0 bs=4M status=progress | gzip > emmc-backup.img.gz
   ```
2. Connect a 3.3 V USB-UART: U-Boot console is UART2 at 115200 8N1.
   Record everything: `picocom -b 115200 /dev/ttyUSB0 | tee boot.log`.

## 1. Base image from SD

Write `out/tinker-edge-r-debian13.img` to an SD card, insert, power on.

| Observation on UART | Meaning / next step |
|---|---|
| nothing at all | wrong UART or baud; try UART0 (stock kernel FIQ debugger) |
| U-Boot prints, then "Kernel load addr" and a hang | the stock U-Boot did not take the mainline `Image`; try the mainline U-Boot fallback (`build-uboot.sh`, `build-image-mainline-uboot.sh`) |
| kernel messages, then "VFS: Cannot open root device" | MMC numbering or a missing built-in driver: check the `mmcblk` names in the log; the stock U-Boot appends `root=/dev/mmcblk0p8` |
| login prompt | success: log in as `debian` / `debian`, set a new password with `passwd` |

Then check: `ip a`, `lsblk`, `lsusb`, `lspci`, `dmesg --level=err,warn`,
HDMI picture, `cat /sys/class/thermal/thermal_zone*/temp`.

## 2. Hardware video (`mpp` variant)

```sh
sudo tinker-boot-select mpp && sudo reboot
ls -l /dev/mpp_service        # appears when rk_vcodec binds
dmesg | grep -i -E "mpp|vcodec|iommu"
```

Then build/run the Rockchip `mpp` test tools (`downloads/mpp`, see its
README) against `/dev/mpp_service`. Report any `iommu` page-fault or
`mpp_` errors verbatim.

## 3. NPU (`npu` or `full` variant)

```sh
sudo tinker-boot-select npu && sudo reboot
dmesg | grep -i npu                         # "NPU powered up"
lsusb                                        # the NPU shows up in loader mode
sudo systemctl status npu-firmware npu-transfer-proxy
cat /run/npu-load.log
```

`upgrade_tool` comes from your ASUS image (extracted at build time). If
`npu-firmware.service` fails, send `/run/npu-load.log` and `dmesg`.

## 4. Roll back

Remove the SD card: the board boots the stock system from eMMC again.
A bad variant on the SD card: `sudo dd if=/var/lib/tinker/boot.prev of=/dev/disk/by-partlabel/boot`
or re-write the SD image.
