Repo root: /home/oleg/Documents/rnd-llm/asus-tinker-edge-r (git-ignored: reference/,
downloads/, build/, out/). Goal: Debian 13 + Linux 6.18 LTS, HW video codec, NPU.

Non-obvious facts (verified from sources/stock image, NOT on hardware):
- Stock ASUS U-Boot loads an Android boot.img (kernel + RSCE resource image
  with rk-kernel.dtb) from GPT partition "boot", and itself appends
  root=/dev/mmcblk0p8 (SD) or mmcblk1p8 (eMMC): rootfs must be partition 8 and
  SD must be mmc0 in the kernel DTS.
- Console: U-Boot uses UART2 @115200 (not 1500000); stock kernel FIQ debugger is UART0.
- NPU is a separate die reached over USB (stock DT has no rockchip,deferred, so
  the ASUS script takes the USB branch); no PCIe patch needed. Power sequence is
  GPIOs + clk_wifi_pmu, done by kernel-mods/.../tinker-npu-power.c.
- BSP develop-6.12 has no buildable RK3399Pro DTS (rk3399pro.dtsi missing); it is
  used only as donor for mpp_service.
- upgrade_tool and ASUS bootloader pieces are not redistributable: extracted from
  the user's ASUS image by scripts/extract-stock-boot.sh.

**Why:** the user is away from the board; work so far is build-time verified only
(QEMU userspace smoke test passes).
**How to apply:** next step is docs/BRINGUP.md when the board arrives; ask for UART
logs. Fallback image uses mainline U-Boot (scripts/build-image-mainline-uboot.sh).

## Agreed plan after 2026-10-13
- User will revisit when the next LTS after 6.18 exists (and once they have the
  board back). Upgrade path: edit LINUX_VERSION/LINUX_SHA256 in sources.lock,
  rebase patches/linux/0001 (IOMMU helpers) and kernel-mods/ (mpp_service port,
  tinker-npu-power) onto the new tree; same-series bumps (6.18.x) are trivial.
- Repo is public: https://github.com/ninja-96/tinker-edge-r-debian (2 commits,
  pushed only when the user asks). First hardware run follows docs/BRINGUP.md.

## Hardware bring-up result (2026-10-23): WORKING
Board boots our Debian 13 + Linux 6.18.56-tinker-edge-r, SSH works (192.168.1.53,
tinker-edge-r.local, debian/debian). What actually works and why:
- Boot chain that works: mainline U-Boot 2026.10 (tinker-edge-r-rk3399pro defconfig,
  u-boot-mods/) with the Rockchip DDR blob rk3399pro_ddr_800MHz_v1.30 (idbloader via
  mkimage rksd ddr:spl) + rkbin BL31 + extlinux on the rootfs partition
  (scripts/build-image-mainline-uboot.sh with UBOOT_BIN=out/u-boot-rockchip-ddrblob.bin).
  Open LPDDR4 TPL (rock-pi-4 timings) hung right after "U-Boot TPL" -> DDR blob needed.
  Memory: 2x2 GB LPDDR4 (host), NPU has its own 2 GB LPDDR3 (schematic block diagram).
  TF-A (open BL31) fails to link with GCC 14 (PMUSRAM overflow by 3928 B): rkbin BL31 used.
- The stock ASUS U-Boot path (Android boot.img, scripts/build-image.sh) never showed a
  sign of life; mainline kernel needs a 2 MiB-aligned start but ASUS U-Boot runs the raw
  Image in place at 2MiB+0x80000 -> mkbootimg.py now adds a 1.5 MiB trampoline (untested).
- Console: UART0 on the 40-pin header, pin 8=board TX, 10=RX, 9=GND, 115200; UART2
  (vendor console) is on the microSD socket pads. Adapter: PL2303 on /dev/ttyUSB0;
  udev rule /etc/udev/rules.d/99-pl2303.rules; stop ModemManager before using it.
- Flashing eMMC: maskrom = short Recovery header J3 at power-on with USB-C data cable to
  the PC (power 12-19 V DC), release J3 after `lsusb -d 2207:` shows it. Use
  out/tools/rk3399_loader.bin (the RK3399, NOT the rk3399pro loader: that one silently
  drops eMMC writes beyond 32 MiB). scripts/flash-emmc.sh writes 16 MiB pieces and
  verifies; scripts/flash-at.sh <lba> <file> for quick loader/boot rewrites.
- Original eMMC backed up in ~/Documents/rnd-llm/asus-tinker-edge-r/emmc-backup.img (+.sha256).
- Not published yet: README/docs changes after the 3rd commit (c8d55fa) are local only.
