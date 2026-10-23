# After flashing: checks and what is verified

Verified on a real board (Tinker Edge R, 4 GB / 16 GB):

| Check | Result |
|---|---|
| U-Boot SPL and U-Boot on the UART0 header pins | works |
| DRAM 4 GiB, PMIC RK809, eMMC, SD controller, Ethernet detected by U-Boot | works |
| Linux 6.18.56 boots from the eMMC through extlinux | works |
| Ethernet (`end0`), DHCP, SSH, mDNS name | works |
| First-boot root filesystem growth, SSH host keys | works |

**Not tested yet:** HDMI output, USB ports, GPU, hardware video (decode and encode),
Wi-Fi/Bluetooth, audio, NPU, suspend, cpufreq/thermal under load. Reports welcome.

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
reboot to apply.

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
