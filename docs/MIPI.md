# MIPI displays and cameras

## What the board has

Three MIPI connectors (schematic `edge_r_schematics.pdf`):

| Connector | Lanes | Use |
|---|---|---|
| DSI0 (MIPI TX0) | 4 | display |
| CSI0 (MIPI RX0) | 4 | camera |
| CSI1 / DSI1 (MIPI TX1/RX1) | 4 | camera or display |

## What ASUS supports (vendor 4.4 device tree)

- Cameras: **OV5647** (Raspberry Pi camera v1.3, I2C1, address 0x36) on CSI0 into ISP0, and
  **IMX219** (Raspberry Pi camera v2.1, I2C2, address 0x10) on CSI1 into ISP1.
  Each camera has extra power and enable GPIOs.
- Displays: DSI to LVDS (**SN65DSI84**) and DSI to eDP (**SN65DSI86**) bridge boards
  (I2C4), with an FT5406 touch controller on I2C2.

## State in this image

Drivers are built as modules (`config/linux-tinker.fragment`): RK3399 ISP (`rkisp1`), CSI
D-PHY, `ov5647`, `imx219`, `ti-sn65dsi83`, `ti-sn65dsi86`, the Raspberry Pi 7" touch panel
and `edt-ft5x06`. **No sensor or panel is described in the device tree yet**, because
mainline drivers need a different description from the vendor one (the vendor power GPIOs
become `regulator-fixed` and `reset-gpios` nodes) and it has to be checked against real
hardware. Until then nothing appears in `/dev/media*` and no DSI connector is created.

## Next steps (need the hardware)

1. Tell which camera or display board is connected.
2. Describe it in the device tree: sensor node on the I2C bus, endpoint to `&mipi_dphy_rx0`
   (CSI0) or `&mipi_dphy_tx1rx1` (CSI1) and `&isp0` / `&isp1`; for displays a panel or bridge
   endpoint on `&mipi_dsi`.
3. Check with `media-ctl -p`, `v4l2-ctl --list-devices`, and `libcamera` or GStreamer.

`scripts/test-board.sh` reports MIPI camera and DSI display as SKIP until they exist.
