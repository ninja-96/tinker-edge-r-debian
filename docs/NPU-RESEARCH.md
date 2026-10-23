# NPU research notes

Findings about the RK3399Pro NPU die and whether its software stack could be
opened. Method: strings and symbols of the firmware shipped with the stock image,
the public Rockchip/NXP/etnaviv repositories (see [LINKS.md](LINKS.md)).
Nothing here was run on hardware.

## What the NPU die is

- A separate die with its own CPU: 2 x Cortex-A35 (`rk3399pro-npu.dtsi` in the
  Rockchip BSP), RK1808-class, its own 2 GB LPDDR3, no storage.
- It runs its own Linux **4.4.185** (built in January 2022, in the firmware
  `boot.img`), loaded into its RAM by the host at every boot
  (`upgrade_tool db/td/wl/rs`, see [NPU.md](NPU.md)).
- Its ramdisk holds BusyBox, `adbd`, `rknn_server` 1.6.0, the Vivante user space
  (`libGAL`, `libOpenVX`, `libVSC`, `librknn_runtime`, Acuity ovxlib 1.1.26) and
  `galcore.ko`.
- The kernel-side NPU driver is Vivante's `galcore`, version
  **6.4.3.5.293908** (`vipgal_6.4.3.5`). The user-space libraries carry the same
  version string; the libraries and the module must match exactly.

## Is there source for it?

| Place | Result |
|---|---|
| Rockchip BSP kernel `develop-4.4` (66 779 files) | no `galcore`/Vivante sources; only RK1808 device trees and clock driver |
| BSP 4.19/5.10/6.1/6.6/6.12 | `drivers/rknpu` is the driver of a different NPU (RK3566/3568/3588) |
| `rockchip-linux/rknpu`, `airockchip/RK3399Pro_npu` | prebuilt `galcore*.ko` and libraries only |
| NXP `linux-imx` (`drivers/mxc/gpu-viv`) | **same code family**: `lf-5.15.y` is 6.4.3.p4.398061 |
| etnaviv `galcore_headers` | ABI headers up to 6.2.4.p3 only |

## Comparison with the NXP source (function and parameter names only)

- 330 of the 336 kernel functions (`gck*`) exported by Rockchip's `galcore.ko` exist
  in NXP's 6.4.3.p4 source (98 %). Rockchip-only: `gckEVENT_AddListEx`,
  `gckHARDWARE_QchannelBypass`, `gckHARDWARE_QchannelPowerOn`,
  `gckKERNEL_SyncVideoMemory`, `gckOS_RequestReservedMemoryArea`,
  `gckOS_ReleaseReservedMemoryArea`.
- 40 of 43 module parameters match (`exclusiveBase`, `exclusiveSize`, `isrPoll` do not).
- **Not compared:** the ioctl ABI (`gcsHAL_INTERFACE`, command codes). The closed
  `libGAL` checks the version string and the structure layouts; compatibility with a
  port of NXP's driver is unknown. `etnaviv/libvivhook` (logs the ioctl traffic of
  `libGAL`) is the tool to find out, on a running NPU.

Conclusion: the kernel side could plausibly be rebuilt from NXP's open driver; the
user space (compiler, runtime, model format) stays closed. A practical gain is small:
about 3 TOPS, RKNN 1.7.x operator set, no LLM.

## Network of the NPU die

`rk3399pro-npu.dtsi` describes: 2 x A35, USB 3.0 (DWC3 + combphy), 2 x I2C, one UART,
DMA, timers, thermal sensor, the NPU. No Ethernet MAC (RK1808 has one, but it is not
wired in the RK3399Pro package), no Wi-Fi. The firmware's `S50usbdevice` can switch the
USB gadget to ADB, RNDIS (Ethernet over USB), ACM serial or NTB (the RKNN transport);
the stock configuration enables NTB only. A shell or a network link to the NPU die is
therefore possible by enabling `ADB_EN`/`RNDIS_EN` in a repacked ramdisk.

## Power and safety

The NPU rail `vdd_npu` is a `tcs4525` regulator on the NPU die's I2C1 (address 0x1c),
750-800 mV, controlled by the NPU's own Linux; its OPP table tops at 880 mV
(`rockchip,max-volt`). A custom NPU image must copy the regulator, OPP and thermal
nodes unchanged from the stock device tree. The firmware lives in RAM only
(nothing persistent is written), so a bad image is cured by a power cycle.
