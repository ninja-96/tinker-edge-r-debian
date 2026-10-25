# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the tinker-edge-r project contributors
"""Python interface to the NPU of the RK3399Pro (ASUS Tinker Edge R).

A thin binding over Rockchip's ``librknn_api.so`` (RKNN API 1.7.x), written with ``ctypes``
so that it works with the system Python (3.13 in Debian 13) without a compiled extension.
The library and the NPU stack behind it are Rockchip's closed binaries; this module only
loads them. The structure layouts follow the public header ``rknn_api.h``.

    import numpy as np
    import rknn3399pro as npu

    print(npu.find_devices())                      # [("RK3399PRO", "e802a990cc82be3a")]
    with npu.RKNN("mobilenet_v1.rknn") as net:
        print(net.inputs[0])                       # name, shape, dtype, quantisation
        x = np.zeros((1, 224, 224, 3), np.uint8)   # NHWC image, as the model wants it
        (scores,) = net.run([x])                   # list of float32 arrays
        print(scores.argmax(), net.last_run_us, "us")

The NPU has to be running first (``systemctl start npu-firmware npu-transfer-proxy``,
which the image does at boot with the ``-npu`` or ``-full`` device tree).
"""
import ctypes
import os

import numpy as np

__version__ = "0.1.0"
__all__ = ["RKNN", "RKNNError", "TensorAttr", "find_devices", "sdk_version", "load_library"]

# ---- constants of rknn_api.h -------------------------------------------------------------
MAX_DIMS, MAX_NAME_LEN, MAX_DEVS, MAX_DEV_LEN = 16, 256, 256, 64

FLAG_PRIOR_HIGH, FLAG_PRIOR_MEDIUM, FLAG_PRIOR_LOW = 0x0, 0x1, 0x2
FLAG_ASYNC, FLAG_COLLECT_PERF, FLAG_LOAD_MODEL_IN_NPU = 0x4, 0x8, 0x10

QUERY_IN_OUT_NUM, QUERY_INPUT_ATTR, QUERY_OUTPUT_ATTR = 0, 1, 2
QUERY_PERF_DETAIL, QUERY_PERF_RUN, QUERY_SDK_VERSION = 3, 4, 5

# tensor type -> numpy dtype (RKNN_TENSOR_FLOAT32, FLOAT16, INT8, UINT8, INT16)
_DTYPES = [np.float32, np.float16, np.int8, np.uint8, np.int16]
_TYPE_OF = {np.dtype(d): i for i, d in enumerate(_DTYPES)}
_FORMATS = ["nchw", "nhwc"]
_QNT = ["none", "dfp", "affine_asymmetric"]

_ERRORS = {
    -1: "RKNN_ERR_FAIL", -2: "RKNN_ERR_TIMEOUT", -3: "RKNN_ERR_DEVICE_UNAVAILABLE",
    -4: "RKNN_ERR_MALLOC_FAIL", -5: "RKNN_ERR_PARAM_INVALID", -6: "RKNN_ERR_MODEL_INVALID",
    -7: "RKNN_ERR_CTX_INVALID", -8: "RKNN_ERR_INPUT_INVALID", -9: "RKNN_ERR_OUTPUT_INVALID",
    -10: "RKNN_ERR_DEVICE_UNMATCH", -11: "RKNN_ERR_INCOMPATILE_PRE_COMPILE_MODEL",
    -12: "RKNN_ERR_INCOMPATILE_OPTIMIZATION_LEVEL_VERSION", -13: "RKNN_ERR_TARGET_PLATFORM_UNMATCH",
}


class RKNNError(RuntimeError):
    """A call into librknn_api failed."""

    def __init__(self, call, code):
        self.code = code
        self.name = _ERRORS.get(code, "unknown error")
        hint = ""
        if code == -3:
            hint = " (is the NPU running? systemctl status npu-firmware npu-transfer-proxy)"
        super().__init__(f"{call} failed: {self.name} ({code}){hint}")


# ---- structures (field by field as in rknn_api.h) ----------------------------------------
_u32, _i32 = ctypes.c_uint32, ctypes.c_int32


class _InOutNum(ctypes.Structure):
    _fields_ = [("n_input", _u32), ("n_output", _u32)]


class _TensorAttr(ctypes.Structure):
    _fields_ = [("index", _u32), ("n_dims", _u32), ("dims", _u32 * MAX_DIMS),
                ("name", ctypes.c_char * MAX_NAME_LEN), ("n_elems", _u32), ("size", _u32),
                ("fmt", _i32), ("type", _i32), ("qnt_type", _i32),
                ("fl", ctypes.c_int8), ("zp", _u32), ("scale", ctypes.c_float)]


class _PerfRun(ctypes.Structure):
    _fields_ = [("run_duration", ctypes.c_int64)]


class _SdkVersion(ctypes.Structure):
    _fields_ = [("api_version", ctypes.c_char * 256), ("drv_version", ctypes.c_char * 256)]


class _DevicesId(ctypes.Structure):
    _fields_ = [("n_devices", _u32), ("types", (ctypes.c_char * MAX_DEV_LEN) * MAX_DEVS),
                ("ids", (ctypes.c_char * MAX_DEV_LEN) * MAX_DEVS)]


class _Input(ctypes.Structure):
    _fields_ = [("index", _u32), ("buf", ctypes.c_void_p), ("size", _u32),
                ("pass_through", ctypes.c_uint8), ("type", _i32), ("fmt", _i32)]


class _Output(ctypes.Structure):
    _fields_ = [("want_float", ctypes.c_uint8), ("is_prealloc", ctypes.c_uint8), ("index", _u32),
                ("buf", ctypes.c_void_p), ("size", _u32)]


class _InitExtend(ctypes.Structure):
    _fields_ = [("device_id", ctypes.c_char_p)]


class _RunExtend(ctypes.Structure):
    _fields_ = [("frame_id", ctypes.c_uint64)]


class _OutputExtend(ctypes.Structure):
    _fields_ = [("frame_id", ctypes.c_uint64)]


# ---- the library -------------------------------------------------------------------------
_lib = None


def load_library(path=None):
    """Load ``librknn_api.so`` (cached). ``path`` or the environment variable RKNN_API_LIB
    overrides the search in the usual places."""
    global _lib
    if _lib is not None and path is None:
        return _lib
    candidates = [path, os.environ.get("RKNN_API_LIB"), "librknn_api.so",
                  "/usr/lib/aarch64-linux-gnu/librknn_api.so", "/usr/lib/librknn_api.so",
                  "/usr/local/lib/librknn_api.so"]
    last = None
    for c in filter(None, candidates):
        try:
            lib = ctypes.CDLL(c)
            break
        except OSError as e:
            last = e
    else:
        raise OSError("librknn_api.so not found (it exists for aarch64 only; set RKNN_API_LIB "
                      f"to its path): {last}")
    P = ctypes.POINTER
    lib.rknn_find_devices.argtypes = [P(_DevicesId)]
    lib.rknn_init.argtypes = [P(ctypes.c_uint64), ctypes.c_void_p, _u32, _u32]
    lib.rknn_init2.argtypes = [P(ctypes.c_uint64), ctypes.c_void_p, _u32, _u32, P(_InitExtend)]
    lib.rknn_destroy.argtypes = [ctypes.c_uint64]
    lib.rknn_query.argtypes = [ctypes.c_uint64, _i32, ctypes.c_void_p, _u32]
    lib.rknn_inputs_set.argtypes = [ctypes.c_uint64, _u32, P(_Input)]
    lib.rknn_run.argtypes = [ctypes.c_uint64, P(_RunExtend)]
    lib.rknn_outputs_get.argtypes = [ctypes.c_uint64, _u32, P(_Output), P(_OutputExtend)]
    lib.rknn_outputs_release.argtypes = [ctypes.c_uint64, _u32, P(_Output)]
    for name in ("find_devices", "init", "init2", "destroy", "query", "inputs_set", "run",
                 "outputs_get", "outputs_release"):
        getattr(lib, "rknn_" + name).restype = ctypes.c_int
    _lib = lib
    return lib


def _check(call, code):
    if code != 0:
        raise RKNNError(call, code)


def find_devices():
    """List of (type, id) of the connected NPUs."""
    lib = load_library()
    d = _DevicesId()
    _check("rknn_find_devices", lib.rknn_find_devices(ctypes.byref(d)))
    return [(d.types[i].value.decode(), d.ids[i].value.decode()) for i in range(d.n_devices)]


def sdk_version():
    """(API version, driver version) as reported through a running context; needs a model,
    so it is also available as ``RKNN.sdk_version``. Without a model the API version of the
    header this module follows is returned."""
    return ("1.7.5", None)


class TensorAttr:
    """Description of one model input or output."""

    def __init__(self, a):
        n = min(a.n_dims, MAX_DIMS)
        self.index = a.index
        self.name = a.name.decode(errors="replace")
        self.dims = tuple(a.dims[:n])
        self.n_elems, self.size = a.n_elems, a.size
        self.fmt = _FORMATS[a.fmt] if 0 <= a.fmt < len(_FORMATS) else str(a.fmt)
        self.dtype = np.dtype(_DTYPES[a.type]) if 0 <= a.type < len(_DTYPES) else None
        self.qnt = _QNT[a.qnt_type] if 0 <= a.qnt_type < len(_QNT) else str(a.qnt_type)
        self.fl, self.zp, self.scale = a.fl, a.zp, a.scale

    @property
    def shape(self):
        """``dims`` when they describe all elements, else a flat shape (the NPU reports
        padded or reordered dims for some outputs)."""
        n = 1
        for d in self.dims:
            n *= d
        return self.dims if n == self.n_elems and n > 0 else (self.n_elems,)

    def __repr__(self):
        return (f"TensorAttr(#{self.index} {self.name!r} shape={self.shape} {self.fmt} "
                f"dtype={self.dtype} qnt={self.qnt} zp={self.zp} scale={self.scale:.6g})")


class RKNN:
    """One model on the NPU. Use as a context manager or call ``close()``."""

    def __init__(self, model, device_id=None, collect_perf=False, priority="high"):
        """``model`` is a path to a .rknn file or the bytes of one."""
        self._lib = load_library()
        self._ctx = ctypes.c_uint64(0)
        self._closed = False
        if isinstance(model, (str, os.PathLike)):
            with open(model, "rb") as f:
                model = f.read()
        self._model = ctypes.create_string_buffer(bytes(model), len(model))   # kept alive
        flags = {"high": FLAG_PRIOR_HIGH, "medium": FLAG_PRIOR_MEDIUM, "low": FLAG_PRIOR_LOW}[priority]
        if collect_perf:
            flags |= FLAG_COLLECT_PERF
        if device_id is None:
            code = self._lib.rknn_init(ctypes.byref(self._ctx), self._model, len(model), flags)
            call = "rknn_init"
        else:
            self._dev = _InitExtend(device_id.encode())
            code = self._lib.rknn_init2(ctypes.byref(self._ctx), self._model, len(model), flags,
                                        ctypes.byref(self._dev))
            call = "rknn_init2"
        _check(call, code)
        n = _InOutNum()
        self._query(QUERY_IN_OUT_NUM, n)
        self.inputs = [self._attr(QUERY_INPUT_ATTR, i) for i in range(n.n_input)]
        self.outputs = [self._attr(QUERY_OUTPUT_ATTR, i) for i in range(n.n_output)]
        self.last_run_us = None

    # -- helpers -------------------------------------------------------------------------
    def _query(self, cmd, struct):
        _check("rknn_query", self._lib.rknn_query(self._ctx, cmd, ctypes.byref(struct),
                                                  ctypes.sizeof(struct)))

    def _attr(self, cmd, index):
        a = _TensorAttr()
        a.index = index
        self._query(cmd, a)
        return TensorAttr(a)

    @property
    def sdk_version(self):
        """(API version, driver version on the NPU)."""
        v = _SdkVersion()
        self._query(QUERY_SDK_VERSION, v)
        return v.api_version.decode(), v.drv_version.decode()

    # -- inference -----------------------------------------------------------------------
    def run(self, inputs, fmt="nhwc", raw=False):
        """Run the model once.

        ``inputs``: one array or a list, one per model input (uint8, int8, int16, float16 or
        float32; the driver converts to what the model needs). ``fmt`` is the layout of the
        image arrays, ``"nhwc"`` (default, how images usually are) or ``"nchw"``.
        Returns a list of arrays, one per output: float32 by default, or in the raw type of the
        model output with ``raw=True``.
        """
        if isinstance(inputs, np.ndarray):
            inputs = [inputs]
        if len(inputs) != len(self.inputs):
            raise ValueError(f"the model has {len(self.inputs)} input(s), got {len(inputs)}")
        arrays = [np.ascontiguousarray(a) for a in inputs]
        arr = (_Input * len(arrays))()
        for i, a in enumerate(arrays):
            t = _TYPE_OF.get(a.dtype)
            if t is None:
                raise TypeError(f"input {i}: dtype {a.dtype} is not supported "
                                "(uint8, int8, int16, float16, float32)")
            arr[i].index, arr[i].buf, arr[i].size = i, a.ctypes.data, a.nbytes
            arr[i].pass_through, arr[i].type, arr[i].fmt = 0, t, _FORMATS.index(fmt)
        _check("rknn_inputs_set", self._lib.rknn_inputs_set(self._ctx, len(arrays), arr))
        _check("rknn_run", self._lib.rknn_run(self._ctx, None))
        n = len(self.outputs)
        out = (_Output * n)()
        for i in range(n):
            out[i].want_float, out[i].is_prealloc, out[i].index = (0 if raw else 1), 0, i
        _check("rknn_outputs_get", self._lib.rknn_outputs_get(self._ctx, n, out, None))
        try:
            results = []
            for i, o in enumerate(self.outputs):
                dt = o.dtype if (raw and o.dtype is not None) else np.dtype(np.float32)
                count = out[i].size // dt.itemsize
                buf = (ctypes.c_char * out[i].size).from_address(out[i].buf)
                a = np.frombuffer(buf, dtype=dt, count=count).copy()
                results.append(a.reshape(o.shape) if a.size == int(np.prod(o.shape)) else a)
        finally:
            self._lib.rknn_outputs_release(self._ctx, n, out)
        try:
            p = _PerfRun()
            self._query(QUERY_PERF_RUN, p)
            self.last_run_us = p.run_duration
        except RKNNError:
            self.last_run_us = None
        return results

    # -- lifetime ------------------------------------------------------------------------
    def close(self):
        if not self._closed:
            self._closed = True
            self._lib.rknn_destroy(self._ctx)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
