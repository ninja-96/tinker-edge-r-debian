# Tinker Edge R: Debian 13 on mainline Linux

Build and flash scripts for an open-source system on the **ASUS Tinker Edge R**
(Rockchip RK3399Pro): Debian 13 "trixie", Linux 6.18 LTS, mainline U-Boot.
Unofficial; not affiliated with ASUS or Rockchip.

Goal of this repository: a user with a board clones it, runs the commands below and
ends up with a working board (boots from the eMMC, network and SSH up).

## What you need

- A Linux PC (x86_64) with **Docker** (your user in the `docker` group), `git`,
  `usbutils`, `sudo`, about 25 GB of free disk and internet access (Linux and U-Boot
  sources, Debian packages).
- The board with its **12-19 V DC power adapter**, a **USB-C cable with data**,
  a jumper or tweezers for header **J3 (Recovery)**, an **Ethernet** cable.
- Recommended: a **3.3 V USB-UART** adapter for the serial console.

## 1. Build the image (about 40 minutes)

```sh
git clone https://github.com/ninja-96/tinker-edge-r-debian.git
cd tinker-edge-r-debian
scripts/make-image.sh
```

Result: `out/tinker-edge-r-debian13.img` (1.6 GB) and the flashing tools in `out/tools/`.
The script downloads the Linux and U-Boot sources and Debian packages, and builds
everything inside a Docker container. Nothing from ASUS is downloaded and no Rockchip
binary is downloaded: the few Rockchip binaries needed are in [`blobs/`](blobs/README.md).
The only Rockchip download is the source of the open-source (Apache-2.0) `mpp` video
library, pinned in `sources.lock`.

## 2. Flash the eMMC over USB

The eMMC is written through the SoC's USB boot mode (maskrom). This **erases the
system on the eMMC**, so a backup comes first.

1. Power off the board. Connect the board's **USB-C port** to the PC (USB 2.0 port,
   no hub). **Short header J3 (Recovery)** with a jumper, then connect the power adapter.
2. Check that the PC sees the board, then **remove the jumper**:
   ```sh
   lsusb -d 2207:        # "RK3399 in Mask ROM mode"
   ```
3. Back up the original eMMC (15 GB, takes a few minutes) and flash the image:
   ```sh
   scripts/backup-emmc.sh emmc-backup.img
   scripts/flash-emmc.sh --backup emmc-backup.img
   ```
   Type `ERASE-EMMC` when asked. The script writes the image in pieces, reads the eMMC
   back and compares it, and resets the board only if everything matches.
4. Disconnect the USB-C cable (J3 stays open) and power-cycle the board.

Optional, once, so that the tools work without `sudo` for USB access:
`sudo cp third_party/rkdeveloptool/99-rk-rockusb.rules /etc/udev/rules.d/ && sudo udevadm control --reload`.

Restore the original system any time:
`scripts/flash-emmc.sh --skip-backup-check emmc-backup.img` (in maskrom mode).
Details and pitfalls: [docs/FLASHING.md](docs/FLASHING.md).

## 3. First boot

Connect the Ethernet cable and wait **about two minutes** (the first boot grows the
root filesystem to the whole eMMC and creates the SSH host keys). Then:

```sh
ssh debian@tinker-edge-r.local        # password: debian
```

If the name does not resolve, look for `tinker-edge-r` in the DHCP client list of
your router and use its IP. **Change the password right away** (`passwd`): SSH is
enabled and the default password is public.

Serial console (optional, for debugging): UART0 on the 40-pin header, pin 8 (board TX),
pin 10 (board RX), pin 9 (GND), 115200 8N1, 3.3 V. See [docs/UART.md](docs/UART.md).

Swap: none on the eMMC (it would wear the flash). On an SD card the first boot creates
a 2 GiB swap file.

## Desktop (KDE Plasma)

The image is a server system without a graphical interface. To install the KDE Plasma
desktop, connect a monitor to **HDMI**, log in over SSH (or on the serial console) and run:

```sh
sudo apt update
sudo apt install task-kde-desktop      # about 3 GB download, needs the 15 GB of the eMMC
sudo reboot
```

The login screen (SDDM) appears on the monitor. The GPU driver is Mesa Panfrost (OpenGL ES
on the Mali-T860, no Vulkan). The board has 4 GB of RAM; if the desktop feels
tight, enable compressed swap in RAM: `sudo apt install systemd-zram-generator`.
Sound goes to the 3.5 mm jack and, with a monitor attached, to HDMI.
To go back to the text console: `sudo systemctl set-default multi-user.target`.
The desktop is **not tested on this board yet**, see [docs/ROADMAP.md](docs/ROADMAP.md).

## Update the kernel

The kernel is built from the kernel.org tarball pinned in `sources.lock`. To move to a
new 6.18.x release: put the new version and checksum into `sources.lock`, rebuild with
`./run.sh scripts/build-kernel.sh` and install it on the running board without
reflashing with `scripts/update-kernel.sh`. Full instructions, including the next LTS
and how to rebase patches: [docs/KERNEL.md](docs/KERNEL.md).

## Test the board

`scripts/test-board.sh` runs a stress test over SSH (CPU, RAM, eMMC, GPU, network,
USB, HDMI, audio, Wi-Fi/Bluetooth, then everything at once) and prints a PASS/FAIL
table: [docs/TESTING.md](docs/TESTING.md).

## Status

Verified on a real board:

| Works | |
|---|---|
| Boot chain: DRAM init, U-Boot SPL and U-Boot, Linux 6.18.56 | yes |
| Ethernet (DHCP), SSH, mDNS name `tinker-edge-r.local`, NTP | yes |
| eMMC install with read-back verification | yes |

Built into the image but **not tested yet**: HDMI, USB ports, GPU (panfrost), hardware
video decode/encode, Wi-Fi/Bluetooth (Realtek firmware from Debian), audio, NPU
(experimental, closed stack, not in the default image). See
[docs/ROADMAP.md](docs/ROADMAP.md) and [docs/BRINGUP.md](docs/BRINGUP.md).

## Open source policy

Only open source lives in this repository. The unavoidable closed binaries are
listed with their origin and licence in [docs/OPEN.md](docs/OPEN.md) and
[blobs/README.md](blobs/README.md): the Rockchip DRAM initialisation blob and BL31
(published unmodified under the Rockchip licence, with a link to where they come from).
Wi-Fi/Bluetooth firmware comes from the Debian `non-free-firmware` packages at
build time. The ASUS TinkerOS image and everything taken from it are never published.

## Repository layout

| Path | Contents |
|---|---|
| `scripts/` | build, flash and test scripts (`make-image.sh` is the entry point) |
| `docker/` | build environment (Debian 13, cross toolchain, QEMU) |
| `dts/` | device trees: base, `-mpp`, `-npu`, `-full` |
| `patches/` | patches to Linux and TF-A |
| `kernel-mods/`, `u-boot-mods/` | new kernel and U-Boot files (`mpp_service` port, NPU power driver, board defconfig) |
| `config/` | kernel configuration fragment |
| `overlay/` | files copied into the rootfs (services, helper scripts) |
| `blobs/` | the Rockchip binaries, unmodified, with licence and origin |
| `third_party/` | `rkdeveloptool` (GPL), copied unmodified |
| `docs/` | documentation and research notes |
| `sources.lock` | pinned versions of the downloaded sources |

## Documentation

[KERNEL](docs/KERNEL.md) · [TESTING](docs/TESTING.md) ·
[BOOT](docs/BOOT.md) · [FLASHING](docs/FLASHING.md) · [UART](docs/UART.md) ·
[HARDWARE](docs/HARDWARE.md) · [BRINGUP](docs/BRINGUP.md) · [VIDEO](docs/VIDEO.md) ·
[NPU](docs/NPU.md) · [NPU-RESEARCH](docs/NPU-RESEARCH.md) · [OPEN](docs/OPEN.md) ·
[HISTORY](docs/HISTORY.md) · [ROADMAP](docs/ROADMAP.md) · [LINKS](docs/LINKS.md)

## Upstream and reference repositories

ASUS:
- https://github.com/TinkerEdgeR (Debian kernel 4.4 and U-Boot of the stock image:
  [debian-kernel](https://github.com/TinkerEdgeR/debian-kernel),
  [debian-u-boot](https://github.com/TinkerEdgeR/debian-u-boot),
  [ALPR](https://github.com/TinkerEdgeR/ALPR))
- https://github.com/TinkerBoard/TinkerBoard/wiki/Tinker-Edge-R (wiki)
- https://tinker-board.asus.com/download-list.html?product=tinker-edge-r (images and documents)

Rockchip:
- https://github.com/rockchip-linux/rkbin (source of `blobs/rockchip/`, commit `3e288fe814e059dd06833495f845cab04ac20a5c`)
- https://github.com/rockchip-linux/rkdeveloptool (source of `third_party/rkdeveloptool`, commit `304f073752fd25c854e1bcf05d8e7f925b1f4e14`)
- https://github.com/rockchip-linux/kernel (BSP kernel, donor of the `mpp_service` port)
- https://github.com/rockchip-linux/u-boot
- https://github.com/rockchip-linux/mpp
- https://github.com/airockchip/RK3399Pro_npu , https://github.com/rockchip-linux/rknpu , https://github.com/rockchip-linux/rknn-toolkit (NPU)

Open upstreams the build downloads: https://www.kernel.org (Linux),
https://github.com/u-boot/u-boot, https://deb.debian.org (Debian). More references
in [docs/LINKS.md](docs/LINKS.md).

## Licenses

Scripts and docs: MIT. Kernel patches, drivers and device trees: GPL-2.0 (device
trees GPL-2.0+ OR MIT). Third-party components keep their own licenses; see
[NOTICE.md](NOTICE.md). The stock ASUS image and its binaries are **not** distributed here.
