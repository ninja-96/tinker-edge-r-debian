#!/usr/bin/env python3
"""Round-trip checks of the image packers: pack a fixture, unpack it with the parser, compare.

    python3 tests/packer_roundtrip.py

Host tool (no board, no container): scripts/mkbootimg.py, npu-open/tools/npu_ramdisk.py,
the RSCE block of scripts/npu-fw-tinker.py. Exits non-zero when a packer is broken.
"""
import gzip
import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = 2048


def run(*cmd):
    r = subprocess.run([sys.executable, *cmd], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise SystemExit("%s failed:\n%s%s" % (" ".join(str(c) for c in cmd), r.stdout, r.stderr))
    return r.stdout


def pad(b, n=PAGE):
    return b + b"\0" * (-len(b) % n)


def boot_header(ksz, rsz, ssz):
    h = bytearray(PAGE)
    h[0:8] = b"ANDROID!"
    struct.pack_into("<II", h, 8, ksz, 0x10008000)
    struct.pack_into("<II", h, 16, rsz, 0x11000000)
    struct.pack_into("<II", h, 24, ssz, 0x10F00000)
    struct.pack_into("<II", h, 32, 0x10000100, PAGE)
    return bytes(h)


def read_rsce(second):
    """(name, data) pairs of a Rockchip resource image."""
    assert second[:4] == b"RSCE", "not RSCE"
    count = struct.unpack("<I", second[12:16])[0]
    out = []
    for i in range(count):
        e = second[512 + 512 * i: 1024 + 512 * i]
        assert e[:4] == b"ENTR", "bad RSCE entry %d" % i
        name = e[4:260].split(b"\0")[0].decode()
        off, size = struct.unpack("<II", e[260:268])
        out.append((name, second[off * 512: off * 512 + size]))
    return out


def cpio_newc(entries):
    """Minimal newc cpio: `entries` are (name, data, mode); a TRAILER entry is appended."""

    def rec(name, data, mode, ino):
        nm = name.encode() + b"\0"   # namesz counts the NUL; the padding after the name is separate
        size = len(data)
        data += b"\0" * ((-len(data)) % 4)
        f = [ino, mode, 0, 0, 1, 0, size, 0, 0, 0, 0, len(nm), 0]
        r = b"070701" + b"".join(b"%08X" % v for v in f) + nm
        r += b"\0" * (-len(r) % 4)   # the data starts 4-aligned after the name
        return r + data

    out = b""
    for n, (name, data, mode) in enumerate(entries, 1):
        out += rec(name, data, mode, n)
    return out + rec("TRAILER!!!", b"", 0, 0)


def mkbootimg_roundtrip(d):
    k = bytes(range(256)) * 16
    dtb = b"\xd0\x0d\x0d\x0d" + b"FAKE-DTB" * 16
    open(os.path.join(d, "k"), "wb").write(k)
    open(os.path.join(d, "d"), "wb").write(dtb)
    run("scripts/mkbootimg.py", "--kernel", os.path.join(d, "k"), "--dtb", os.path.join(d, "d"), "-o", os.path.join(d, "boot.img"))
    img = open(os.path.join(d, "boot.img"), "rb").read()
    ksz = struct.unpack_from("<I", img, 8)[0]
    ssz = struct.unpack_from("<I", img, 24)[0]
    assert img[:8] == b"ANDROID!"
    assert ksz == 0x180000 + len(k), "the trampoline pad is missing"
    k0 = PAGE
    ker = img[k0:k0 + ksz]
    assert struct.unpack("<I", ker[:4])[0] == 0x14000000 | (0x180000 // 4), "no b #TRAMPOLINE_PAD"
    assert ker[0x180000:] == k, "the kernel is damaged by the trampoline"
    up = lambda n: (n + PAGE - 1) // PAGE * PAGE
    s0 = k0 + up(ksz)
    second = img[s0:s0 + ssz]
    names = dict(read_rsce(second))
    assert names == {"rk-kernel.dtb": dtb}, "the RSCE does not hold the dtb"
    print("mkbootimg.py: ok")


def npu_ramdisk_roundtrip(d):
    kern = b"K" * 5000
    cpio = cpio_newc([("usr", b"", 0o40755), ("usr/bin", b"", 0o40755), ("init", b"#!/bin/sh\necho hello\n", 0o100755)])
    ram = gzip.compress(cpio, 9, mtime=0)
    second = b"\0" * 1024
    img = boot_header(len(kern), len(ram), len(second)) + pad(kern) + pad(ram) + pad(second)
    open(os.path.join(d, "in.img"), "wb").write(img)
    hello = b"echo added\n"
    open(os.path.join(d, "hello"), "wb").write(hello)

    run("npu-open/tools/npu_ramdisk.py", os.path.join(d, "in.img"), os.path.join(d, "out.img"), "--add", "usr/bin/hello=%s:755" % os.path.join(d, "hello"))
    run("npu-open/tools/npu_ramdisk.py", os.path.join(d, "out.img"), "--extract", "usr/bin/hello=%s" % os.path.join(d, "got_hello"), "--extract", "init=%s" % os.path.join(d, "got_init"))
    assert open(os.path.join(d, "got_hello"), "rb").read() == hello, "--add / --extract is broken"
    assert open(os.path.join(d, "got_init"), "rb").read() == b"#!/bin/sh\necho hello\n", "an untouched file changed"

    out = open(os.path.join(d, "out.img"), "rb").read()
    ksz = struct.unpack_from("<I", out, 8)[0]
    rsz = struct.unpack_from("<I", out, 16)[0]
    assert out[PAGE:PAGE + ksz] == kern, "the kernel must be copied unchanged"
    up = lambda n: (n + PAGE - 1) // PAGE * PAGE
    r0 = PAGE + up(ksz)
    parts = (out[PAGE:PAGE + ksz], out[r0:r0 + rsz], out[r0 + up(rsz):r0 + up(rsz) + 1024])
    h = hashlib.sha1()
    for p, n in ((parts[0], ksz), (parts[1], rsz), (parts[2], 1024)):
        h.update(p)
        h.update(struct.pack("<I", n))
    assert out[0x240:0x260] == h.digest().ljust(32, b"\0"), "the sha1 in the header is wrong"
    ls = run("npu-open/tools/npu_ramdisk.py", os.path.join(d, "out.img"), "--list")
    assert "hello" in ls and "init" in ls
    run("npu-open/tools/npu_ramdisk.py", os.path.join(d, "out.img"), os.path.join(d, "out2.img"), "--remove", "usr/bin/hello")
    assert "hello" not in run("npu-open/tools/npu_ramdisk.py", os.path.join(d, "out2.img"), "--list")
    print("npu_ramdisk.py: ok")


def rsce_roundtrip():
    p = os.path.join(ROOT, "scripts", "npu-fw-tinker.py")
    spec = importlib.util.spec_from_file_location("npu_fw_tinker", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    hdr = (b"RSCE" + struct.pack("<HHBBBBI", 0, 0, 1, 1, 1, 0, 2)).ljust(512, b"\0")
    entries = [("rk-kernel.dtb", b"D" * 100), ("second.bin", b"S" * 600)]
    r = mod.rsce_build(hdr, entries)
    got = mod.rsce_dtb(r)
    assert got == entries, "RSCE round-trip changed the entries"
    assert r[4 * 512:4 * 512 + 100] == b"D" * 100, "the data must start at block 4"
    print("npu-fw-tinker.py RSCE: ok")


def main():
    with tempfile.TemporaryDirectory() as d:
        mkbootimg_roundtrip(d)
        npu_ramdisk_roundtrip(d)
        rsce_roundtrip()
    print("packers: ok")


main()
