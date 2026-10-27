# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the tinker-edge-r project contributors
"""TensorFlow Lite on the NPU of the RK3399Pro (ASUS Tinker Edge R).

Loads one TFLite model onto the NPU and runs it. The model stays on the board's side only as a file: the NPU die runs our small server
(`npu_server`) with the open TIM-VX / TFLite VX delegate stack; this module talks to it over its own USB bulk channel. System Python 3.10+,
numpy and nothing else.

    import numpy as np
    import rk3399pro_npu as npu

    with npu.Model("mobilenet_v2_quant.tflite") as net:     # seconds the first time, a fraction of a second after (compiled graph is cached)
        print(net.inputs[0])                                # Tensor('input' (1, 224, 224, 3) uint8 scale=0.0078 zp=128)
        (scores,) = net.exec(x)                             # x: a numpy array in the model's dtype; floats are quantised for you
        print(net.last_invoke_us, "us on the NPU")
        probs = net.outputs[0].dequantize(scores)           # back to float32

One model per NPU. A model the NPU cannot run is refused with `NpuError` (and the NPU stays usable: the model runs in a child process
of the server). Integer models (uint8, int8) run on the NPU's fast path; float32 and float16-weight models work but are slow (about 480 ms
for MobileNet V2); dynamic-range (hybrid) models are not supported. See /usr/share/doc/tinker-edge-r/NPU.md.
"""
import ctypes
import hashlib
import os
import struct
import threading
import time

import numpy as np

from ._usb import NpuError, _Usb, find_devices

__version__ = "0.2.0"
__all__ = ["Model", "NpuModel", "NpuError", "Tensor", "find_devices", "info", "__version__"]

MAGIC = 0x3155504E
T_HELLO, T_LOAD, T_RUN, T_UNLOAD, T_PING, T_LOAD_NBG = 1, 2, 3, 4, 5, 6
HDR = struct.Struct("<IHHII")
TENSOR = struct.Struct("<BBH6ifiI32s")   # struct npu_tensor
# TfLiteType -> numpy
_DT = {1: np.float32, 2: np.int32, 3: np.uint8, 4: np.int64, 6: np.bool_, 7: np.int16, 9: np.int8, 10: np.float16, 11: np.float64}


class Tensor:
    """A model input or output: name, shape, dtype and the quantisation (scale, zero_point; scale 0 for float tensors)."""

    def __init__(self, raw):
        t, nd, _r, *dims, scale, zp, nbytes, name = TENSOR.unpack(raw)
        self.dtype = np.dtype(_DT.get(t, np.uint8))
        self.type_code = t
        self.shape = tuple(dims[:nd])
        self.scale, self.zero_point, self.nbytes = scale, zp, nbytes
        self.name = name.split(b"\0")[0].decode(errors="replace")

    def dequantize(self, a):
        """A quantised array of this tensor as float32 (a float tensor is returned as it is)."""
        a = np.asarray(a)
        return ((a.astype(np.float32) - self.zero_point) * self.scale) if self.scale else a.astype(np.float32)

    def quantize(self, a):
        """A float array as the integer dtype of this tensor (a float tensor is only converted)."""
        a = np.asarray(a)
        if not (self.scale and self.dtype in (np.uint8, np.int8, np.int16)):
            return a.astype(self.dtype)
        info = np.iinfo(self.dtype)
        return np.clip(np.round(a / self.scale) + self.zero_point, info.min, info.max).astype(self.dtype)

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

    # ---- transport ----
    def close_link(self):
        """Stop the reader and release the USB interface."""
        self._closed = True
        try:
            self._send(T_PING, b"bye", self._next_tag())   # the answer wakes the reader, which then ends
        except Exception:
            pass
        self._reader.join(5)
        self._usb.close()

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


class Model(NpuLink):
    def __init__(self, model, load_timeout=240, cache_dir=None, max_inflight=2, async_gap_us=300):
        """model: path to a .tflite file or its bytes. The first load of a model builds the graph on the NPU (13 s for MobileNet V2 uint8, about a minute
        for an int8 model with per-channel weights); the compiled graph is kept in cache_dir (default ~/.cache/npu-models, environment NPU_CACHE;
        cache_dir=\"\" switches the cache off) and the next loads take a fraction of a second."""
        data = bytes(model) if isinstance(model, (bytes, bytearray, memoryview)) else open(model, "rb").read()
        NpuLink.__init__(self, max_inflight)
        self._async_gap = async_gap_us / 1e6   # see async_exec
        try:
            self.hello = self._hello()
            if not self.hello["chip_ok"]:
                raise NpuError("unexpected NPU chip %x/%x/%x: this software is made for 8000/7100/45080001 only" % self.hello["chip"])
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
        """Run once and return the outputs: a list of numpy arrays in the dtype of the model (`self.outputs[i].dequantize(a)` gives float32).
        x is an array (or a list of arrays for several inputs); a float array for an integer input is quantised with the scale of the input."""
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
        """Unload the model: the NPU is free for the next one."""
        if getattr(self, "_closed", True):
            return
        try:
            self._call(T_UNLOAD, timeout=10)
        except Exception:
            pass
        self.close_link()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


NpuModel = Model   # the name of the research prototype


def info():
    """Ask the server on the NPU die for its state without loading a model: {chip, chip_ok, driver, mem_total_kb, mem_avail_kb, model_loaded}.
    Fails with NpuError if another process holds the NPU (a loaded model owns the USB channel)."""
    link = NpuLink()
    try:
        return link._hello()
    finally:
        link.close_link()
