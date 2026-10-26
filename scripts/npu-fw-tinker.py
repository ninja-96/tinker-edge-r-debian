#!/usr/bin/env python3
"""Make Rockchip's NPU boot.img (airockchip/RK3399Pro_npu) work on the ASUS Tinker Edge R.

Rockchip's image is made for their EVB, where the NPU is powered through two regulators on
I2C (tcs452x at 0x1c, syr837 at 0x40). The Tinker Edge R has one fan53555 at 0x60 instead, so
the NPU kernel fails with "galcore: failed to get regulator" and the NPU resets as soon as a
model is loaded. This script replaces those two nodes in the device tree inside the image.
Only the device tree changes (kernel and ramdisk are copied as they are).

    npu-fw-tinker.py boot.img boot-tinker.img          (needs dtc)
"""
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
import os

PAGE_FALLBACK = 2048
NODE = '''	vdd-npu {
		compatible = "regulator-fixed";
		regulator-name = "vdd_npu";
		regulator-min-microvolt = <0xcf850>;
		regulator-max-microvolt = <0xcf850>;
		regulator-always-on;
		regulator-boot-on;
		phandle = <0x41>;
	};

'''


def dtc(args, data):
    with tempfile.TemporaryDirectory() as t:
        i, o = os.path.join(t, "in"), os.path.join(t, "out")
        open(i, "wb").write(data)
        subprocess.run(["dtc", *args, "-o", o, i], check=True, stderr=subprocess.DEVNULL)
        return open(o, "rb").read()


def drop_node(text, name):
    """Remove the node 'name { ... };' (balanced braces) from the text."""
    m = re.search(r"^\t\t%s \{\n" % re.escape(name), text, re.M)
    if not m:
        raise SystemExit("node %s not found: is this Rockchip's NPU image?" % name)
    depth, i = 0, m.end() - 2
    while True:
        c = text[i]
        depth += (c == "{") - (c == "}")
        i += 1
        if depth == 0:
            break
    end = text.index("\n", i) + 1
    return text[:m.start()] + text[end:], m.start()


def patch_dts(text):
    if "vdd-npu {" in text:
        raise SystemExit("already a Tinker Edge R image")
    text, _ = drop_node(text, "tcs452x@1c")
    text, _ = drop_node(text, "syr837@40")
    at = text.index("\ti2c@ff500000 {\n")
    text = text[:at] + NODE + text[at:]
    text = re.sub(r"^\t\tnpu_1-supply = <0x42>;\n", "", text, flags=re.M)
    text = re.sub(r"^\t\tvdd_npu_1 = .*\n", "", text, flags=re.M)
    text = text.replace('vdd_npu = "/i2c@ff500000/tcs452x@1c";', 'vdd_npu = "/vdd-npu";')
    text = text.replace('model = "Rockchip RK3399pro-npu EVB V10 Board";', 'model = "Tinker Edge R NPU";')
    return text


def rsce_dtb(rsce):
    """Entries of the resource image (512-byte blocks): [(name, data)] and the index of the dtb."""
    assert rsce[:4] == b"RSCE"
    count = struct.unpack("<I", rsce[12:16])[0]
    out = []
    for i in range(count):
        e = rsce[512 + 512 * i: 1024 + 512 * i]
        assert e[:4] == b"ENTR"
        name = e[4:260].split(b"\0")[0].decode()
        off, size = struct.unpack("<II", e[260:268])
        out.append((name, rsce[off * 512: off * 512 + size]))
    return out


def rsce_build(header, entries):
    first = 1 + len(entries)               # blocks used by the header and the entry table
    first = max(first, 4)                  # the stock images start the data at block 4
    blob, table, block = b"", b"", first
    for name, data in entries:
        e = b"ENTR" + name.encode().ljust(256, b"\0") + struct.pack("<II", block, len(data))
        table += e.ljust(512, b"\0")
        padded = data + b"\0" * (-len(data) % 512)
        blob += padded
        block += len(padded) // 512
    head = header[:512]
    return (head + table).ljust(first * 512, b"\0") + blob


def main(src, dst):
    img = open(src, "rb").read()
    magic, ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page = struct.unpack("<8s8I", img[:40])
    assert magic == b"ANDROID!"
    page = page or PAGE_FALLBACK
    up = lambda n: (n + page - 1) // page * page
    kofs = page
    rofs = kofs + up(ksz)
    sofs = rofs + up(rsz)
    kernel, ramdisk = img[kofs:kofs + ksz], img[rofs:rofs + rsz]
    second = img[sofs:sofs + ssz]
    entries = rsce_dtb(second)
    new = []
    for name, data in entries:
        if name == "rk-kernel.dtb":
            text = dtc(["-I", "dtb", "-O", "dts"], data).decode()
            data = dtc(["-I", "dts", "-O", "dtb"], patch_dts(text).encode())
        new.append((name, data))
    second2 = rsce_build(second, new)
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, second2):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    hdr = bytearray(img[:page])
    struct.pack_into("<I", hdr, 24, len(second2))
    hdr[0x240:0x260] = sha.digest().ljust(32, b"\0")
    out = bytes(hdr) + kernel.ljust(up(ksz), b"\0") + ramdisk.ljust(up(rsz), b"\0") + second2.ljust(up(len(second2)), b"\0")
    out += b"\0" * (len(img) - len(out)) if len(img) > len(out) else b""
    open(dst, "wb").write(out)
    print("%s: device tree %d -> %d bytes, image %d bytes" % (dst, len(rsce_dtb(second)[0][1]), len(new[0][1]), len(out)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
