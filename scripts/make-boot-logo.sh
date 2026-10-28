#!/usr/bin/env bash
# Boot logo images from artwork/boot-logo/debian-openlogo-white.svg (the white Debian swirl), for a black screen, centred at every stage:
#   out/boot-logo/u_boot_logo.bmp            U-Boot (8 bit BMP, 300 px high; drawn centred by patches/u-boot/0001)
#   out/boot-logo/logo_linux_clut224.ppm     the kernel's boot logo (plain PPM, at most 224 colours, 300 px high; fbcon=logo-pos:center)
#   out/boot-logo/plymouth-logo.png          Plymouth (1024 px high, transparent; the theme scales it to 40 % of the screen height)
# Run in the builder container (rsvg-convert from librsvg2-bin, netpbm):  ./run.sh scripts/make-boot-logo.sh
set -euo pipefail
cd "$(dirname "$0")/.."
SVG=artwork/boot-logo/debian-openlogo-white.svg
O=out/boot-logo
mkdir -p "$O"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
rsvg-convert -h 300 --background-color=black -o "$T/300.png" "$SVG"
pngtopnm "$T/300.png" > "$T/300.ppm"
pnmquant 224 "$T/300.ppm" 2>/dev/null > "$T/300-224.ppm"
ppmtobmp -bpp 8 "$T/300-224.ppm" 2>/dev/null > "$O/u_boot_logo.bmp"
pnmtopnm -plain "$T/300-224.ppm" > "$O/logo_linux_clut224.ppm"
rsvg-convert -h 1024 -o "$O/plymouth-logo.png" "$SVG"
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$O"
ls -l "$O"
