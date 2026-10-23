# 40-pin header

The header is 3.3 V only. Pin functions follow the ASUS pin table
([wiki](https://github.com/TinkerBoard/TinkerBoard/wiki/Tinker-Edge-R), section
"GPIO config table"). Device tree: end of `dts/rk3399pro-tinker-edge-r.dts`.

| Pin | Name | Function in this image | Linux device |
|---|---|---|---|
| 1, 17 | 3.3 V | power | |
| 2, 4 | 5 V | power | |
| 6, 9, 14, 20, 25, 30, 34, 39 | GND | | |
| 3 | GPIO2_B1 | I2C6 SDA | `/dev/i2c-6` |
| 5 | GPIO2_B2 | I2C6 SCL | `/dev/i2c-6` |
| 7 | GPIO2_D1 | GPIO | chip 2, line 25 |
| 8 | GPIO2_C1 | UART0 TX (serial console) | `/dev/ttyS0` |
| 10 | GPIO2_C0 | UART0 RX (serial console) | `/dev/ttyS0` |
| 11 | GPIO2_C3 | GPIO (UART0 RTS, see below) | chip 2, line 19 |
| 12 | GPIO3_D0 | GPIO (I2S0 SCLK) | chip 3, line 24 |
| 13 | GPIO2_C5 | SPI5 TX (MOSI) | `/dev/spidev5.0` |
| 15 | GPIO2_C4 | SPI5 RX (MISO) | `/dev/spidev5.0` |
| 16 | GPIO2_C6 | SPI5 CLK | `/dev/spidev5.0` |
| 18 | GPIO2_C7 | SPI5 CS0 | `/dev/spidev5.0` |
| 19 | GPIO1_B0 | SPI1 TX (MOSI) | `/dev/spidev1.0` |
| 21 | GPIO1_A7 | SPI1 RX (MISO) | `/dev/spidev1.0` |
| 22 | GPIO3_D4 | GPIO (I2S0 SDO3) | chip 3, line 28 |
| 23 | GPIO1_B1 | SPI1 CLK | `/dev/spidev1.0` |
| 24 | GPIO1_B2 | SPI1 CS0 | `/dev/spidev1.0` |
| 26 | GPIO0_A6 | PWM3A | `/sys/class/pwm` |
| 27 | GPIO2_A7 | I2C7 SDA | `/dev/i2c-7` |
| 28 | GPIO2_B0 | I2C7 SCL | `/dev/i2c-7` |
| 29 | GPIO3_D6 | GPIO (I2S0 SDO1) | chip 3, line 30 |
| 31 | GPIO3_D5 | GPIO (I2S0 SDO2) | chip 3, line 29 |
| 32 | GPIO4_C2 | PWM0 | `/sys/class/pwm` |
| 33 | GPIO4_C6 | PWM1 | `/sys/class/pwm` |
| 35 | GPIO3_D1 | GPIO (I2S0 RX) | chip 3, line 25 |
| 36 | GPIO2_C2 | GPIO (UART0 CTS, see below) | chip 2, line 18 |
| 37 | GPIO4_C5 | GPIO (SPDIF TX) | chip 4, line 21 |
| 38 | GPIO3_D3 | GPIO (I2S1 SDI) | chip 3, line 27 |
| 40 | GPIO3_D7 | GPIO (I2S1 SDO) | chip 3, line 31 |

GPIO line numbers: bank `GPIOn_Xm` is `gpiochip n`, line `(X - A) * 8 + m` (`B2` is line 10).

## Using it

```sh
sudo apt install gpiod i2c-tools                  # GPIO and I2C tools
sudo gpioset -c gpiochip2 25=1                    # pin 7 high (v2 syntax; Ctrl-C releases the line)
gpioget -c gpiochip3 24                           # read pin 12
sudo i2cdetect -y 6                               # scan I2C6 (pins 3, 5)
ls /dev/spidev*                                   # SPI devices
echo 0 | sudo tee /sys/class/pwm/pwmchip0/export  # PWM channel; then set period, duty_cycle, enable
```

Your user is in the `dialout`, `video`, `audio` groups; for GPIO, I2C, SPI and PWM without
`sudo` add udev rules or use `sudo`.

## Choices made in the device tree

- **UART0 RTS/CTS (pins 11, 36) are plain GPIO.** With hardware flow control and an
  unconnected CTS pin the console would stop transmitting.
- **UART4 and SPI1 share pins 19 and 21.** SPI1 is enabled. To get UART4 instead, disable
  `&spi1` and enable `&uart4` in the device tree.
- I2S0 and SPDIF pins stay GPIO; the on-board audio uses I2S1 (3.5 mm jack) and HDMI.
- SPI nodes use the compatible `rohm,dh2228fv`, which is how mainline lets the generic
  `spidev` driver claim a node.

## Testing

`scripts/test-board.sh` drives every plain GPIO pin high and low, scans I2C6 and I2C7,
checks the SPI device nodes and starts the three PWM channels. For an electrical check
put jumper wires between pairs of plain GPIO pins and run, for example,
`GPIO_LOOPBACK="7:29 31:37" scripts/test-board.sh` (pin pairs with a wire between them).
The test cannot check the 3.3 V level, I2C or SPI traffic with real devices, or PWM
output with a scope.
