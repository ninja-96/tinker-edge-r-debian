# After flashing: checks and what is verified

Verified on a real board (Tinker Edge R, 4 GB / 16 GB), release 0.1:

| Check | Result |
|---|---|
| U-Boot SPL and U-Boot on the UART0 header pins | works |
| DRAM 4 GiB, PMIC RK809, eMMC, Ethernet detected by U-Boot | works |
| Linux 6.18.56 boots from the eMMC through extlinux (`mpp` device tree) | works |
| Ethernet (`end0`), DHCP, SSH, mDNS name, NTP | works |
| First-boot root filesystem growth, SSH host keys, locale `en_US.UTF-8` | works |
| eMMC HS200: read 164 MB/s, write 26-52 MB/s, no CRC errors | works |
| cpufreq (A53 to 1.4 GHz, A72 to 1.8 GHz), CPU and RAM stress, 47 C at full load | works |
| GPU: Panfrost OpenGL ES 3.1, Rusticl OpenCL 3.0 | works |
| Hardware H.264 / VP8 / JPEG encode, H.264 decode, GStreamer elements | works |
| Wi-Fi and Bluetooth adapters up with firmware; Wi-Fi scan; RK809 audio; RTC | works (link level) |
| 40-pin header: GPIO, I2C, SPI nodes, PWM | works (software side) |
| NPU: power-up, firmware load, ADB, RKNN inference (optional closed files) | works |

**Not tested yet:** HDMI picture, USB-A / USB-C with devices, SD card boot, KDE, Bluetooth
pairing, Wi-Fi connection, network throughput, suspend. **Not supported:** DisplayPort over
USB-C, Vulkan, MIPI panels and cameras, LTE modem. See [ROADMAP.md](ROADMAP.md).

## Quick checks on the board

```sh
uname -r                                  # 6.18.56-tinker-edge-r
df -h /                                   # grown to the eMMC size (about 14 GB)
ip -br a                                  # end0 with an address
dmesg --level=err,warn | head -40
lsusb; lsblk
cat /sys/class/thermal/thermal_zone*/temp
journalctl -b -p warning --no-pager | head -40
```

Device tree variants (all in `/boot/dtbs/`): `sudo tinker-boot-select [base|mpp|npu|full]`,
reboot to apply. The image boots `mpp`; `full` adds the NPU; `base` uses the mainline V4L2 decoders.

An automatic check of everything above: `scripts/test-board.sh` ([TESTING.md](TESTING.md)).

## When the board does not come up

1. Open the serial console ([UART.md](UART.md)) and power-cycle: U-Boot should print
   within a second. Nothing at all: wiring/adapter first.
2. U-Boot appears but the kernel does not: press a key during the countdown and
   use the U-Boot shell, `run bootcmd`, `ls mmc 0:1 /boot`.
3. Kernel up but no network: `dmesg | grep -i -E "eth|stmmac|phy|link"`, check the cable
   and the link LED on the connector.
4. Re-flash through maskrom ([FLASHING.md](FLASHING.md)); the BootROM cannot be bricked
   by a bad image.

## Safety notes

- Power the board from the 12-19 V adapter, not from a USB port.
- The default login is `debian` / `debian` and SSH is on; change the password at once.
