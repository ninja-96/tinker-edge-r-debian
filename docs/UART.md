# Serial console

| Adapter | Board (40-pin header) |
|---|---|
| RX | pin 8 (board TX) |
| TX | pin 10 (board RX) |
| GND | pin 9 |
| VCC | not connected |

3.3 V logic, 115200 8N1. Verified: U-Boot SPL, U-Boot and the kernel print here.

```sh
sudo systemctl stop ModemManager        # it probes ttyUSB ports with AT commands
sudo screen /dev/ttyUSB0 115200         # exit: Ctrl+A, K, y
# or: picocom -b 115200 /dev/ttyUSB0 | tee boot.log
```

Permissions: add yourself to `dialout`, or a udev rule for the adapter, e.g. for a
PL2303 (`067b:2303`): `SUBSYSTEM=="tty", ATTRS{idVendor}=="067b", ATTRS{idProduct}=="2303", MODE="0666"`.
Only one program may open the port (a second `screen` blocks others).

If nothing appears: swap RX/TX, check the ground, check that the adapter is 3.3 V,
check that pin 1 is where you think it is (5 V on pins 2 and 4), and test the adapter
alone by shorting its TX and RX and typing.

## What a good boot looks like

```
U-Boot TPL 2026.10 ...           (only with the all-open variant)
U-Boot SPL 2026.10 ...
Trying to boot from MMC1
U-Boot 2026.10 ...
DRAM:  4 GiB
PMIC:  RK809
...
Hit any key to stop autoboot
...kernel messages...
tinker-edge-r login:
```

Press a key during the countdown to get the U-Boot shell (`=>`), where `ls mmc 0:1 /boot`,
`printenv`, `run bootcmd` and manual `load` + `booti` help to debug.
