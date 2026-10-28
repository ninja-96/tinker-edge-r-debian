# Tinker Edge R hardware notes

Sources: ASUS schematics, wiki and Quick Start Guide (see [LINKS.md](LINKS.md)),
the stock device tree, and what was observed on the board.

| Item | Value |
|---|---|
| SoC | Rockchip RK3399Pro: RK3399 (2 x Cortex-A72 + 4 x Cortex-A53) plus a separate NPU die |
| Memory (host) | 2 x 2 GB LPDDR4 (one chip per channel), 4 GB total; the image runs it at 933 MHz (LPDDR4-1866), ASUS used 800 MHz ([BOOT.md](BOOT.md)) |
| Memory (NPU die) | 2 GB LPDDR3, its own |
| Storage | 16 GB eMMC, microSD slot |
| Ethernet | Gigabit, Realtek RTL8211F over RGMII |
| Wi-Fi/Bluetooth | M.2 Key E connector, Realtek RTL8822CE module (Wi-Fi on PCIe x1, `rtw88_8822ce`; Bluetooth RTL8822CU on USB, `btusb`). Two U.FL (IPEX MHF1) antenna connectors on the module: **Wi-Fi and Bluetooth need antennas** (dual band 2.4 / 5 GHz); without them they reach only centimetres (Bluetooth audio drops, see TESTING.md) |
| PMIC | RK809 (with audio codec), plus two FAN53555-type regulators for CPU big cluster and GPU |
| Power | 12-19 V DC adapter (Quick Start Guide) |
| Video | HDMI, DisplayPort over USB-C, MIPI DSI0, CSI0, CSI1/DSI1 (4 lanes each, see [MIPI.md](MIPI.md)) |
| Mini PCIe slot | for a 4G LTE modem (USB 2.0 host0 lines) with a nano-SIM holder wired to it; **no modem is fitted** |
| RTC | RK809 built-in RTC (`rtc0`), `RTC_BATTERY` header for a backup battery |
| USB-C controller | FUSB302 on I2C8, address 0x22, interrupt GPIO1_A2 (`fcs,fusb302`, TCPM): a 5 V source and USB host. VBUS goes through a TI TPS2553 power switch (current limit about 1.5 A, short-circuit and thermal protection). **The GPIO that enables it depends on the board revision**: GPIO2_D2 on revision 1.02 (ADC channel 5 at about 1.85 V; the vendor tree calls it `vbus2-5v`), GPIO4_D1 on revision 1.01 (about 0.9 V, `vbus-5v`). **This image is made for revision 1.02** (see "Board revision" below): on 1.01 USB-C would give no power and GPIO2_D2 (there possibly the Bluetooth wake line, by a comment in the vendor tree) would be driven instead. PCIe CLKREQ# is GPIO4_D0 (`pcie_clkreqnb_cpm`), not GPIO2_D2 |
| 40-pin header | see [GPIO.md](GPIO.md) |
| Recovery header | J3 ("Recovery"): shorted at power-on enters maskrom |
| USB-C | data port used for maskrom flashing |

## Serial console

| Signal | Header pin |
|---|---|
| UART0 TX (board output) | 8 |
| UART0 RX (board input) | 10 |
| GND | 9 (also 6, 14, 20, 25, 30, 34, 39) |

115200 8N1, 3.3 V logic (do not connect the adapter's VCC). The ASUS wiki calls
pins 8/10 the "console log port". UART2, the console of the ASUS U-Boot, is wired to
the **microSD socket pads** (`SDMMC0_D0/UART2_RX`, `SDMMC0_D1/UART2_TX`), so it is
not reachable on the header; this is why the stock bootloader looked silent.

Header pin numbering: pin 1 (3.3 V) and 2 (5 V) are at one end; odd pins are one row,
even pins the other. Check with a multimeter: pins 2 and 4 carry 5 V, pins 6 and 9
are ground.

## NPU

A separate die with its own CPU (2 x Cortex-A35), its own memory and its own
Linux (4.4.185), loaded from the host at every boot. On this board it is reached
over USB. See [NPU.md](NPU.md) and [NPU-RESEARCH.md](NPU-RESEARCH.md).

## Interesting numbers

| Item | Value |
|---|---|
| eMMC area rewritten by the board itself | around sector 7168 (3.5 MiB): vendor storage; ignore when comparing images |
| USB IDs in maskrom | `2207:330c` ("RK3399 in Mask ROM mode") |
| USB name of the loader | "USB-MSC", manufacturer "RockChip" |

## What the ASUS device tree describes but the board does not have

The stock device tree was derived from the RK3399Pro EVB, so it lists chips that are
not on this board. An I2C scan of the running board finds only: RK809 (0x20) and the
CPU regulator (0x60) on I2C0; four EEPROM addresses 0x50-0x53 on I2C1; FUSB302 (0x22)
and the GPU regulator (0x60) on I2C8. Not present: the MPU6500 accelerometer/gyroscope,
the AK8963 compass, the BQ25703 charger and the camera sensors (cameras are plug-in).
Worth knowing about: the on-board FUSB302 and the 4 KiB EEPROM on I2C1 (an `at24`
device, `/sys/bus/nvmem/devices/1-00501/nvmem`).

## Board revision

The image is made for and tested on **revision 1.02** of the board. The revision is set by a resistor divider on ADC channel 5 (the vendor U-Boot reads it):
`cat /sys/bus/iio/devices/iio:device0/in_voltage5_raw` times 1.8066 gives millivolts.

| ADC channel 5 | Revision | Known difference |
|---|---|---|
| 1700-1900 mV (raw about 1022) | 1.02 | USB-C VBUS switch on GPIO2_D2 (what this image uses) |
| 800-1000 mV | 1.01 | USB-C VBUS switch on GPIO4_D1: USB-C gives no power with this image |
| 1100-1300 mV, below 100 mV | 4, 3 (named so by the vendor U-Boot) | not seen; the vendor tree treats them like 1.02 |

