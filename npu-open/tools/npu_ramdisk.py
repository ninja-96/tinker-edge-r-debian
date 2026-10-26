#!/usr/bin/env python3
"""Rebuild the ramdisk inside an Android boot.img of the NPU firmware, without root (pure Python cpio newc).

    npu_ramdisk.py IN_boot.img OUT_boot.img [--list] [--extract /path=file]... [--add /path=file[:mode]]... [--remove /path]...

--extract writes a file (a symlink is resolved) of the ramdisk to disk; --add replaces a file or adds one (mode octal, default 0755); --remove deletes a file or a symlink.
Kernel and device tree are copied unchanged. Research tool (rnd/verisilicon).
"""
import argparse
import gzip
import hashlib
import os
import struct
import sys

MAGIC = b"070701"


def parse_cpio(data):
    """newc archive -> ordered list of dicts."""
    out, pos = [], 0
    while True:
        assert data[pos:pos + 6] == MAGIC, "not a newc cpio at %d" % pos
        f = [int(data[pos + 6 + 8 * i: pos + 14 + 8 * i], 16) for i in range(13)]
        ino, mode, uid, gid, nlink, mtime, size, dmaj, dmin, rmaj, rmin, namesz, _chk = f
        pos += 110
        name = data[pos:pos + namesz - 1].decode()
        pos += namesz
        pos = (pos + 3) & ~3
        body = data[pos:pos + size]
        pos += size
        pos = (pos + 3) & ~3
        if name == "TRAILER!!!":
            return out
        out.append(dict(ino=ino, mode=mode, uid=uid, gid=gid, nlink=nlink, mtime=mtime, dmaj=dmaj, dmin=dmin,
                        rmaj=rmaj, rmin=rmin, name=name, data=body))


def write_cpio(entries):
    buf = bytearray()

    def rec(e, ino):
        name = e["name"].encode() + b"\0"
        hdr = MAGIC + b"".join(b"%08x" % v for v in (
            ino, e["mode"], e["uid"], e["gid"], e["nlink"], e["mtime"], len(e["data"]),
            e["dmaj"], e["dmin"], e["rmaj"], e["rmin"], len(name), 0))
        buf.extend(hdr + name)
        buf.extend(b"\0" * (-len(buf) % 4))
        buf.extend(e["data"])
        buf.extend(b"\0" * (-len(buf) % 4))

    for n, e in enumerate(entries, 1):
        rec(e, n)
    rec(dict(mode=0, uid=0, gid=0, nlink=1, mtime=0, dmaj=0, dmin=0, rmaj=0, rmin=0, name="TRAILER!!!", data=b""), 0)
    buf.extend(b"\0" * (-len(buf) % 512))
    return bytes(buf)


def split_boot(img):
    magic, ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page = struct.unpack("<8s8I", img[:40])
    assert magic == b"ANDROID!"
    up = lambda n: (n + page - 1) // page * page
    k0 = page
    r0 = k0 + up(ksz)
    s0 = r0 + up(rsz)
    return dict(page=page, kernel=img[k0:k0 + ksz], ramdisk=img[r0:r0 + rsz], second=img[s0:s0 + ssz], header=img[:page])


def build_boot(parts, ramdisk):
    page = parts["page"]
    up = lambda n: (n + page - 1) // page * page
    hdr = bytearray(parts["header"])
    struct.pack_into("<I", hdr, 16, len(ramdisk))
    sha = hashlib.sha1()
    for p in (parts["kernel"], ramdisk, parts["second"]):
        sha.update(p)
        sha.update(struct.pack("<I", len(p)))
    hdr[0x240:0x260] = sha.digest().ljust(32, b"\0")
    return bytes(hdr) + parts["kernel"].ljust(up(len(parts["kernel"])), b"\0") + ramdisk.ljust(up(len(ramdisk)), b"\0") + \
        parts["second"].ljust(up(len(parts["second"])), b"\0")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", action="append", default=[])
    ap.add_argument("--add", action="append", default=[])
    ap.add_argument("--remove", action="append", default=[])
    a = ap.parse_args()
    img = open(a.src, "rb").read()
    parts = split_boot(img)
    entries = parse_cpio(gzip.decompress(parts["ramdisk"]))
    if a.list:
        for e in entries:
            print("%06o %8d %s" % (e["mode"], len(e["data"]), e["name"]))
    byname = {e["name"]: e for e in entries}
    for spec in a.extract:
        path, _, out = spec.partition("=")
        e = byname.get(path.lstrip("/"))
        for _ in range(8):   # follow symbolic links
            if e is not None and (e["mode"] & 0o170000) == 0o120000:
                tgt = e["data"].decode()
                e = byname.get(os.path.normpath(os.path.join(os.path.dirname(e["name"]), tgt)).lstrip("/"))
            else:
                break
        if e is None:
            sys.exit("--extract: %s is not in the ramdisk" % path)
        open(out, "wb").write(e["data"])
    for r in a.remove:
        r = r.lstrip("/")
        if r not in byname:
            sys.exit("--remove: %s is not in the ramdisk" % r)
        entries.remove(byname.pop(r))
    for spec in a.add:
        path, _, src = spec.partition("=")
        src, _, mode = src.partition(":")
        path = path.lstrip("/")
        data = open(src, "rb").read()
        mode = 0o100000 | int(mode or "755", 8)
        if path in byname:
            byname[path]["data"], byname[path]["mode"] = data, mode
        else:
            parent = os.path.dirname(path)
            if parent and parent not in byname:
                sys.exit("--add: directory %s is not in the ramdisk" % parent)
            e = dict(mode=mode, uid=0, gid=0, nlink=1, mtime=0, dmaj=0, dmin=0, rmaj=0, rmin=0, name=path, data=data)
            entries.append(e)
            byname[path] = e
    if not a.dst:
        return
    ramdisk = gzip.compress(write_cpio(entries), 9, mtime=0)
    out = build_boot(parts, ramdisk)
    # same size as the input (the loader writes the file at a fixed place): pad with zeros if smaller
    if len(out) < len(img):
        out += b"\0" * (len(img) - len(out))
    open(a.dst, "wb").write(out)
    print("%s: ramdisk %d -> %d bytes, %d files, image %d bytes" % (a.dst, len(parts["ramdisk"]), len(ramdisk), len(entries), len(out)))


main()
