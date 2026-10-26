"""Host side of the open NPU stack (research prototype, rnd/verisilicon): a class that loads one TFLite model onto the NPU of the
RK3399Pro over our own USB bulk channel and runs it. System Python 3.10+, numpy, libusb-1.0 (ctypes), nothing else.

    npu = NpuModel("mobilenet_v2_1.0_224_quant.tflite")        # loads and prepares the model on the NPU (seconds)
    print(npu.inputs, npu.outputs)
    out = npu.exec(x)                                            # synchronous: list of numpy arrays, one per output
    npu.async_exec(x, callback)                                  # asynchronous: callback(outputs, error) is called when the result arrives
    npu.close()                                                  # (or `del`) unloads the model: the NPU is free again

Protocol: server/protocol.h. One model per NPU: a second NpuModel raises an error.
"""
import ctypes
import hashlib
import os
import struct
import threading
import time
import time

import numpy as np

MAGIC = 0x3155504E
T_HELLO, T_LOAD, T_RUN, T_UNLOAD, T_PING, T_LOAD_NBG = 1, 2, 3, 4, 5, 6
HDR = struct.Struct("<IHHII")
TENSOR = struct.Struct("<BBH6ifiI32s")   # struct npu_tensor
# TfLiteType -> numpy
_DT = {1: np.float32, 2: np.int32, 3: np.uint8, 4: np.int64, 6: np.bool_, 7: np.int16, 9: np.int8, 10: np.float16, 11: np.float64}


class NpuError(RuntimeError):
    pass


class _Usb:
    """The vendor interface 0xff / 0x4e / 0x50 of the NPU die (2207:0019): one bulk OUT and one bulk IN endpoint, used through usbfs
    (ioctl USBDEVFS_BULK): every thread blocks in its own system call, there is no shared event loop as in libusb's synchronous API
    (two threads in it, one reading without end, one writing, hung transfers and then the xHCI of this SoC)."""

    USBDEVFS_BULK = 0xC0185502
    USBDEVFS_CLAIMINTERFACE = 0x8004550F
    USBDEVFS_RELEASEINTERFACE = 0x80045510
    USBDEVFS_DISCONNECT_CLAIM = 0x8108551B   # not needed: nothing else binds this interface

    def __init__(self):
        import fcntl
        self._fcntl = fcntl
        self.fd = None
        base = "/sys/bus/usb/devices"
        for d in sorted(os.listdir(base)):
            try:
                if open(os.path.join(base, d, "idVendor")).read().strip() == "2207" and open(os.path.join(base, d, "idProduct")).read().strip() == "0019":
                    bus = int(open(os.path.join(base, d, "busnum")).read())
                    dev = int(open(os.path.join(base, d, "devnum")).read())
                    raw = open(os.path.join(base, d, "descriptors"), "rb").read()
                    break
            except OSError:
                continue
        else:
            raise NpuError("NPU (USB 2207:0019) not found; is the NPU firmware loaded (systemctl status npu-firmware)?")
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
            raise NpuError("no access to /dev/bus/usb/%03d/%03d: run as root or add a udev rule for 2207:0019" % (bus, dev))
        try:
            fcntl.ioctl(self.fd, self.USBDEVFS_CLAIMINTERFACE, struct.pack("I", self.iface))
        except OSError as e:
            os.close(self.fd)
            raise NpuError("cannot claim the USB interface (%s): another program holds it (the stock npu_transfer_proxy? stop npu-transfer-proxy; "
                           "a second model in this or another process?)" % e)

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
            import errno
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


class Tensor:
    def __init__(self, raw):
        t, nd, _r, *dims, scale, zp, nbytes, name = TENSOR.unpack(raw)
        self.dtype = np.dtype(_DT.get(t, np.uint8))
        self.type_code = t
        self.shape = tuple(dims[:nd])
        self.scale, self.zero_point, self.nbytes = scale, zp, nbytes
        self.name = name.split(b"\0")[0].decode(errors="replace")

    def __repr__(self):
        q = " scale=%.6g zp=%d" % (self.scale, self.zero_point) if self.scale else ""
        return "Tensor(%r %s %s%s)" % (self.name, self.shape, self.dtype, q)


class NpuLink:
    """The transport: messages over the bulk channel, a reader thread that matches replies to requests by tag."""

    def __init__(self, max_inflight=2):
        self._usb = _Usb()
        # at most this many RUN requests are on their way: the server reads only a few, and an OUT transfer that waits on the bus for the NPU is
        # what the xHCI of this SoC was seen to choke on; async_exec waits (back-pressure) when the limit is reached
        self._slots = threading.BoundedSemaphore(max(1, max_inflight))
        self._lock = threading.Lock()
        self._tag_lock = threading.Lock()
        self._tag = 0
        self._pending = {}
        self._closed = False
        self._rx = bytearray()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

class NpuModel(NpuLink):
    def __init__(self, model, load_timeout=120, cache_dir=None, max_inflight=2, async_gap_us=300):
        """model: path to a .tflite file or its bytes. The first load of a model prepares the graph on the NPU (about 13 s for MobileNet);
        the compiled graph is kept in cache_dir (default ~/.cache/npu-models, environment NPU_CACHE) and the next loads take a fraction of a second."""
        data = bytes(model) if isinstance(model, (bytes, bytearray, memoryview)) else open(model, "rb").read()
        NpuLink.__init__(self, max_inflight)
        self._async_gap = async_gap_us / 1e6   # see async_exec
        try:
            self.hello = self._hello()
            if not self.hello["chip_ok"]:
                raise NpuError("unexpected NPU chip %x/%x/%x" % (self.hello["chip"]))
            self.from_cache = False
            cache = cache_dir if cache_dir is not None else os.environ.get("NPU_CACHE", os.path.expanduser("~/.cache/npu-models"))
            key = hashlib.sha256(data).hexdigest() + "." + self.hello["driver"].replace("/", "_") + "." + "%x_%x_%x" % self.hello["chip"]
            path = os.path.join(cache, key + ".nbg") if cache else None
            reply = None
            if path and os.path.exists(path):
                try:
                    reply = self._call(T_LOAD_NBG, open(path, "rb").read(), timeout=load_timeout)
                    self.from_cache = True
                except NpuError as e:
                    print("the cached graph was refused (%s): building it again" % e)
            if reply is None:
                reply = self._call(T_LOAD, data, timeout=load_timeout)
        except Exception:
            self.close()
            raise
        n_in, n_out, self.load_ms, self.mem_used_kb = struct.unpack_from("<IIII", reply, 0)
        t = [Tensor(reply[16 + i * TENSOR.size:16 + (i + 1) * TENSOR.size]) for i in range(n_in + n_out)]
        tail = 16 + (n_in + n_out) * TENSOR.size
        nbg_len = struct.unpack_from("<I", reply, tail)[0]
        if nbg_len and path and not self.from_cache:
            os.makedirs(cache, exist_ok=True)
            tmp = path + ".tmp%d" % os.getpid()
            with open(tmp, "wb") as f:
                f.write(struct.pack("<II", n_in, n_out) + reply[16:tail] + reply[tail + 4:tail + 4 + nbg_len])
            os.replace(tmp, path)
        self.inputs, self.outputs = t[:n_in], t[n_in:]

    # ---- transport ----
    def _next_tag(self):
        with self._tag_lock:   # two threads must never get the same tag: the second waiter would replace the first, which then waits for ever
            self._tag = (self._tag + 1) & 0xFFFFFFFF
            return self._tag

    def _send(self, typ, payload, tag):
        payload = bytes(payload)
        pad = (HDR.size + len(payload)) % 1024 == 0   # see NPU_FLAG_PAD: never end a stream on a multiple of the packet size
        self._usb.write(HDR.pack(MAGIC, typ, 2 if pad else 0, tag, len(payload)) + payload + (b"\0" if pad else b""))

    def _read_loop(self):
        buf = ctypes.create_string_buffer(64 * 1024)
        rx = bytearray()
        while not self._closed:
            try:
                n = self._usb.read_some(buf, 0)   # no timeout: a cancelled transfer is a 'stop endpoint' command and the xHCI of this SoC can hang on a storm of them
            except NpuError as e:
                self._fail_all(str(e))
                return
            if n:
                rx += buf.raw[:n]
            if self._closed and not n:
                return
            while len(rx) >= HDR.size:
                magic, typ, flags, tag, ln = HDR.unpack_from(rx)
                if magic != MAGIC:
                    # stray bytes (the rest of a message whose start went to a dead transfer of an earlier process): skip to the next header
                    k = rx.find(struct.pack("<I", MAGIC), 1)
                    del rx[:k if k > 0 else len(rx) - 3]
                    self.resync_bytes = getattr(self, "resync_bytes", 0) + (k if k > 0 else len(rx))
                    continue
                if len(rx) < HDR.size + ln:
                    break
                total = HDR.size + ln + (1 if flags & 2 else 0)
                if len(rx) < total:
                    break
                payload = bytes(rx[HDR.size:HDR.size + ln])
                del rx[:total]
                with self._lock:
                    waiter = self._pending.pop(tag, None)
                if waiter:
                    waiter(None if flags & 1 else payload, payload.decode(errors="replace") if flags & 1 else None)
            if self._closed:
                return

    def _fail_all(self, msg):
        with self._lock:
            waiters, self._pending = list(self._pending.values()), {}
        for w in waiters:
            try:
                w(None, msg)
            except Exception:
                pass

    def _call(self, typ, payload=b"", timeout=30):
        done = threading.Event()
        box = {}

        def cb(data, err):
            box["data"], box["err"] = data, err
            done.set()
        tag = self._next_tag()
        with self._lock:
            self._pending[tag] = cb
        self._send(typ, payload, tag)
        if not done.wait(timeout):
            with self._lock:
                self._pending.pop(tag, None)
            raise NpuError("the NPU did not answer in %s s" % timeout)
        if box["err"]:
            raise NpuError(box["err"])
        return box["data"]

    def _hello(self):
        for attempt in range(4):
            try:
                d = self._call(T_HELLO, timeout=3)
                break
            except NpuError as e:
                if attempt == 3 or "did not answer" not in str(e):
                    raise
        
        proto, model, rev, prod, ok, total, avail, loaded = struct.unpack_from("<8I", d, 0)
        return dict(proto=proto, chip=(model, rev, prod), chip_ok=ok, mem_total_kb=total, mem_avail_kb=avail, model_loaded=loaded,
                    driver=d[32:64].split(b"\0")[0].decode())

    # ---- running ----
    def _pack(self, x):
        if isinstance(x, np.ndarray):
            x = [x]
        if len(x) != len(self.inputs):
            raise NpuError("the model has %d input(s), %d given" % (len(self.inputs), len(x)))
        out = []
        for a, t in zip(x, self.inputs):
            a = np.asarray(a)
            if a.dtype != t.dtype:
                if np.issubdtype(a.dtype, np.floating) and t.scale and t.dtype in (np.uint8, np.int8, np.int16):
                    info = np.iinfo(t.dtype)
                    a = np.clip(np.round(a / t.scale) + t.zero_point, info.min, info.max).astype(t.dtype)   # quantise a float input
                else:
                    a = a.astype(t.dtype)
            if a.nbytes != t.nbytes:
                raise NpuError("input %s: %d bytes, the model wants %d (shape %s)" % (t.name, a.nbytes, t.nbytes, t.shape))
            out.append(np.ascontiguousarray(a).tobytes())
        return b"".join(out)

    def _unpack(self, d):
        self.last_invoke_us = struct.unpack_from("<I", d, 0)[0]
        off, res = 4, []
        for t in self.outputs:
            res.append(np.frombuffer(d, t.dtype, t.nbytes // t.dtype.itemsize, off).reshape(t.shape).copy())
            off += t.nbytes
        return res

    def exec(self, x, timeout=30):
        """Run once and return the outputs (a list of numpy arrays in the dtype of the model: dequantise with out.scale / zero_point)."""
        payload = self._pack(x)
        self._slots.acquire()
        try:
            return self._unpack(self._call(T_RUN, payload, timeout))
        finally:
            self._slots.release()

    def async_exec(self, x, callback):
        """EXPERIMENTAL. Queue a run and return at once; callback(outputs, error) is called from a helper thread when the result is there.
        A request sent right after a reply (as pipelining does) was seen to hang the USB transfer, and with it the xHCI of the board, in about
        every other run: a pause of async_gap_us (default 300 us) before each request avoids it (3000 frames in a row, 135 FPS against
        about 105 FPS of exec). Without the pause it can reach 159 FPS, at the risk of a stuck bus. exec() is not affected."""
        payload = self._pack(x)
        self._slots.acquire()
        if self._async_gap:
            time.sleep(self._async_gap)

        def cb(data, err):
            self._slots.release()
            try:
                callback(None if err else self._unpack(data), err)
            except Exception as e:   # a failing callback must not stop the reader
                print("callback raised", e)
        tag = self._next_tag()
        with self._lock:
            self._pending[tag] = cb
        self._send(T_RUN, payload, tag)

    def close(self):
        if getattr(self, "_closed", True):
            return
        try:
            self._call(T_UNLOAD, timeout=10)
        except Exception:
            pass
        self._closed = True
        try:
            self._send(T_PING, b"bye", self._next_tag())   # the answer wakes the reader, which then ends
        except Exception:
            pass
        self._reader.join(5)
        self._usb.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
