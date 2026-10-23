#!/usr/bin/env python3
"""Pack an arm64 Image + DTB into the Android boot.img layout used by the
ASUS/Rockchip U-Boot of the Tinker Edge R.

Layout (as in the stock image): header page, raw kernel, no ramdisk, and a
Rockchip resource image (RSCE) as the "second" stage that holds rk-kernel.dtb.

    mkbootimg.py --kernel Image --dtb board.dtb -o boot.img
"""
import argparse
import struct

# The ASUS/Rockchip U-Boot runs a raw Image in place at (load address + page size),
# which is 0x80000 past a 2 MiB boundary (the text_offset convention of the old
# 4.4 kernel). A mainline kernel has text_offset 0 and must start exactly on a
# 2 MiB boundary, otherwise it hangs silently before enabling the MMU. So the
# kernel is placed TRAMPOLINE_PAD bytes after the entry point; the first word is a
# branch to it. 0x180000 + 0x80000 = 2 MiB.
TRAMPOLINE_PAD = 0x180000

PAGE = 2048
KERNEL_ADDR = 0x10008000
SECOND_ADDR = 0x10F00000
TAGS_ADDR = 0x10000100
BLOCK = 512


def pad(data: bytes, align: int) -> bytes:
    return data + b"\0" * (-len(data) % align)


def resource_image(files):
    """Rockchip RSCE: 1 header block, 1 block per index entry, then data."""
    n = len(files)
    header = struct.pack("<4sHHBBBBI", b"RSCE", 0, 0, 1, 1, 1, 0, n)
    table = b""
    data = b""
    offset = (1 + n) * BLOCK
    for name, blob in files:
        blocks = (len(blob) + BLOCK - 1) // BLOCK
        entry = b"ENTR" + name.encode().ljust(256, b"\0")
        entry += struct.pack("<II", offset // BLOCK, len(blob))
        table += entry.ljust(BLOCK, b"\0")
        data += pad(blob, BLOCK)
        offset += blocks * BLOCK
    return header.ljust(BLOCK, b"\0") + table + data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--dtb", required=True)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--no-trampoline", dest="trampoline", action="store_false",
                    help="put the kernel right after the header (U-Boot booti/extlinux)")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    kernel = open(a.kernel, "rb").read()
    if a.trampoline:
        # b #TRAMPOLINE_PAD, x0 (the DTB pointer) is left untouched
        branch = 0x14000000 | (TRAMPOLINE_PAD // 4)
        kernel = struct.pack("<I", branch).ljust(TRAMPOLINE_PAD, b"\0") + kernel
    second = resource_image([("rk-kernel.dtb", open(a.dtb, "rb").read())])

    header = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!",
        len(kernel), KERNEL_ADDR,
        0, 0,                     # no ramdisk
        len(second), SECOND_ADDR,
        TAGS_ADDR, PAGE,
        0, 0,                     # header version, os version
        b"", a.cmdline.encode(), b"", b"",
    )
    img = pad(header, PAGE) + pad(kernel, PAGE) + pad(second, PAGE)
    open(a.output, "wb").write(img)
    print(f"{a.output}: kernel {len(kernel)} B, second {len(second)} B, total {len(img)} B")


main()
