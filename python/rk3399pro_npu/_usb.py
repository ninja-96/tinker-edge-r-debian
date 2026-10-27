# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the tinker-edge-r project contributors
"""The USB side: the vendor interface of the NPU die (class 0xff / 0x4e / 0x50, USB id 2207:0019) through usbfs."""
import ctypes
import errno
import fcntl
import os
import struct

NPU_USB_ID = ("2207", "0019")


class NpuError(RuntimeError):
    """The NPU does not answer, refuses a model, or cannot be reached."""


def find_devices():
    """The NPU dies that are on the USB bus: a list of dicts {bus, dev, serial}. Empty if the firmware is not loaded."""
    base, found = "/sys/bus/usb/devices", []
    try:
        names = sorted(os.listdir(base))
    except OSError:
        return found
    for d in names:
        try:
            rd = lambda n: open(os.path.join(base, d, n)).read().strip()
            if (rd("idVendor"), rd("idProduct")) == NPU_USB_ID:
                try:
                    serial = rd("serial")
                except OSError:
                    serial = ""
                found.append({"bus": int(rd("busnum")), "dev": int(rd("devnum")), "serial": serial, "sysfs": d})
        except OSError:
            continue
    return found


class _Usb:
    """The vendor interface 0xff / 0x4e / 0x50 of the NPU die (2207:0019): one bulk OUT and one bulk IN endpoint, used through usbfs
    (ioctl USBDEVFS_BULK): every thread blocks in its own system call, there is no shared event loop as in libusb's synchronous API
    (two threads in it, one reading without end, one writing, hung transfers and then the xHCI of this SoC)."""

    USBDEVFS_BULK = 0xC0185502
    USBDEVFS_CLAIMINTERFACE = 0x8004550F
    USBDEVFS_RELEASEINTERFACE = 0x80045510
    USBDEVFS_DISCONNECT_CLAIM = 0x8108551B   # not needed: nothing else binds this interface

    def __init__(self):
        self._fcntl = fcntl
        self.fd = None
        devs = find_devices()
        if not devs:
            raise NpuError("NPU (USB 2207:0019) not found; is the NPU firmware loaded (systemctl status npu-firmware)?")
        bus, dev = devs[0]["bus"], devs[0]["dev"]
        raw = open(os.path.join("/sys/bus/usb/devices", devs[0]["sysfs"], "descriptors"), "rb").read()
        i, cur, self.iface, eps = 0, None, None, []
        raw = raw[18:]   # skip the device descriptor
        while i + 2 <= len(raw):
            ln, typ = raw[i], raw[i + 1]
            if typ == 4:
                cur = (raw[i + 2], raw[i + 5], raw[i + 6], raw[i + 7])
                if cur[1:] == (0xFF, 0x4E, 0x50):
                    self.iface = cur[0]
            elif typ == 5 and cur and cur[1:] == (0xFF, 0x4E, 0x50):
                eps.append(raw[i + 2])
            i += ln or 1
        if self.iface is None or len(eps) != 2:
            raise NpuError("the NPU does not offer the open channel (interface 0xff/0x4e/0x50): its firmware is not ours")
        self.ep_out = next(e for e in eps if not e & 0x80)
        self.ep_in = next(e for e in eps if e & 0x80)
        try:
            self.fd = os.open("/dev/bus/usb/%03d/%03d" % (bus, dev), os.O_RDWR)
        except PermissionError:
            raise NpuError("no access to /dev/bus/usb/%03d/%03d: the image gives it to the group plugdev (udev rule 60-tinker-npu.rules); log in again after being added to the group, or run as root" % (bus, dev))
        try:
            fcntl.ioctl(self.fd, self.USBDEVFS_CLAIMINTERFACE, struct.pack("I", self.iface))
        except OSError as e:
            os.close(self.fd)
            raise NpuError("cannot claim the USB interface (%s): another model is loaded in this or another process (one model per NPU)" % e)

    class _BulkTransfer(ctypes.Structure):
        _fields_ = [("ep", ctypes.c_uint), ("len", ctypes.c_uint), ("timeout", ctypes.c_uint), ("data", ctypes.c_void_p)]

    _libc = ctypes.CDLL(None, use_errno=True)
    _libc.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
    _libc.ioctl.restype = ctypes.c_int

    def _bulk(self, ep, buf, length, timeout):
        """USBDEVFS_BULK: returns the number of bytes moved; the call (and its wait) is made without the GIL."""
        bt = self._BulkTransfer(ep, length, timeout, ctypes.addressof(buf))
        r = self._libc.ioctl(self.fd, self.USBDEVFS_BULK, ctypes.byref(bt))
        if r < 0:
            e = ctypes.get_errno()
            raise OSError(e, os.strerror(e))
        return r

    def write(self, data, timeout=60000):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        sent = 0
        while sent < len(data):
            n = min(len(data) - sent, 256 * 1024)
            try:
                r = self._bulk(self.ep_out, ctypes.c_char.from_buffer(buf, sent), n, timeout)
            except OSError as e:
                raise NpuError("USB write failed (%s)" % e)
            if r <= 0:
                raise NpuError("USB write failed (%d)" % r)
            sent += r

    def read_some(self, buf, timeout):
        """Blocks until the NPU sends something (timeout 0: without end); returns the byte count."""
        try:
            return self._bulk(self.ep_in, buf, len(buf), timeout)
        except OSError as e:
            if e.errno == errno.ETIMEDOUT:
                return 0
            raise NpuError("USB read failed (%s)" % e)

    def close(self):
        if self.fd is not None:
            try:
                self._fcntl.ioctl(self.fd, self.USBDEVFS_RELEASEINTERFACE, struct.pack("I", self.iface))
            except OSError:
                pass
            os.close(self.fd)
            self.fd = None
