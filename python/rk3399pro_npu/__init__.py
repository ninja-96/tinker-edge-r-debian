# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the tinker-edge-r project contributors
"""TensorFlow Lite on the NPU of the RK3399Pro (ASUS Tinker Edge R).

The NPU die runs our small server (`npu_server`) with the open TIM-VX / TFLite VX delegate stack and LiteRT 2.3.2 (TensorFlow Lite). This package is a thin
layer over the C++ library librk3399pro_npu (module `_core`, pybind11). Several models can be loaded at once and used by several programs: a model
stays on the NPU until it is stopped, and opening the same .tflite file again attaches to the loaded model (no second load). System Python 3.13,
numpy and nothing else.

    import numpy as np
    import rk3399pro_npu as npu

    net = npu.Model("mobilenet_v2_quant.tflite")   # loads it (seconds the first time, a fraction of a second from the cache) or attaches to it
    print(net.inputs[0])                           # Tensor('input' (1, 224, 224, 3) uint8 scale=0.0078 zp=128)
    (scores,) = net.exec(x)                        # x: a numpy array in the model's dtype; floats are quantised for you
    probs = net.outputs[0].dequantize(scores)      # back to float32
    net.close()                                    # this program lets go of it; the model stays loaded for the next one

    report = net.autotune(x)                       # once: the fastest of NPU / CPU / mixed with the same results; later loads use it
    for m in npu.models():                         # what is loaded: id, name, plan, memory, runs ...
        print(m)
    npu.stop("mobilenet_v2_quant.tflite")          # unload by name, id, Model, or "all"

Programs reach the NPU through the daemon `npu-daemon` (socket /run/npu/npu.sock, group plugdev); without it (NPU_DIRECT=1, or the daemon does
not run) one program at a time talks to the NPU over USB itself.

Where a model runs (its plan): integer models (uint8, int8) on the NPU, float models on the CPU of the NPU die (TensorFlow Lite with XNNPACK on its two
Cortex-A35): this NPU runs float16 on 2 of its 10 cores only and is slower than that CPU for them. Operators the NPU cannot do run on that CPU too.
Model.autotune() measures the alternatives and keeps the fastest one whose results match. A model the NPU cannot run is refused with `NpuError`, and
the NPU stays usable: every model runs in a child process of the server. See /usr/share/doc/tinker-edge-r/NPU.md.
"""
import os

import numpy as np

from . import _core
from ._core import NpuError

__version__ = "0.3.0"
__all__ = ["Model", "NpuModel", "NpuError", "Tensor", "ModelInfo", "OpStat", "models", "stop", "find_devices", "info", "is_quantized", "__version__"]


class Tensor:
    """A model input or output.

    name, shape (tuple), dtype (numpy dtype), scale and zero_point (the quantisation; scale is 0 for a float tensor), nbytes, type_code (the TfLiteType)."""

    def __init__(self, ti):
        self.name, self.shape, self.dtype = ti.name, tuple(ti.shape), np.dtype(ti.dtype)
        self.scale, self.zero_point, self.nbytes, self.type_code = ti.scale, ti.zero_point, ti.bytes, ti.type

    def dequantize(self, a):
        """A quantised array of this tensor as float32: (a - zero_point) * scale (a float tensor is returned as float32 unchanged)."""
        a = np.asarray(a)
        return ((a.astype(np.float32) - self.zero_point) * self.scale) if self.scale else a.astype(np.float32)

    def quantize(self, a):
        """A float array as the integer dtype of this tensor: round(a / scale) + zero_point, clipped (a float tensor is only converted)."""
        a = np.asarray(a)
        if not (self.scale and self.dtype in (np.uint8, np.int8, np.int16)):
            return a.astype(self.dtype)
        info = np.iinfo(self.dtype)
        return np.clip(np.round(a / self.scale) + self.zero_point, info.min, info.max).astype(self.dtype)

    def __repr__(self):
        q = " scale=%.6g zp=%d" % (self.scale, self.zero_point) if self.scale else ""
        return "Tensor(%r %s %s%s)" % (self.name, self.shape, self.dtype, q)


class OpStat:
    """An operator type of a model (Model.ops()): code (TfLiteBuiltinOperator), name ("CONV_2D", ...), total, on_npu (the rest runs on the CPU of the NPU die)."""

    def __init__(self, o):
        self.code, self.name, self.total, self.on_npu = o.code, o.name, o.total, o.on_npu

    def __repr__(self):
        return "OpStat(%s: %d, %d on the NPU)" % (self.name, self.total, self.on_npu)


class ModelInfo:
    """A model loaded on the NPU, as models() lists it.

    id: the number the server gave it; name: the name it was loaded with (the file name by default); key: sha256 of the .tflite file;
    plan: where it runs ("npu", "npu-nosplit", "cpu", "mixed:<operator codes on the CPU>");
    inputs / outputs: the tensor counts; mem_used_mb: memory it takes on the NPU die; load_ms: duration of its load on the NPU;
    runs: requests served; age_s: seconds since the load; from_cache: loaded from a compiled graph."""

    def __init__(self, mi):
        self.id, self.name, self.key = mi.id, mi.name, mi.key
        self.inputs, self.outputs, self.load_ms = mi.inputs, mi.outputs, mi.load_ms
        self.mem_used_mb = mi.mem_used_kb / 1024
        self.runs, self.age_s, self.from_cache, self.plan = mi.runs, mi.age_s, mi.from_cache, mi.plan

    def __repr__(self):
        return "ModelInfo(id=%d %r, %s, %d in / %d out, %.0f MB, %d runs, loaded %d s ago%s)" % (
            self.id, self.name, self.plan, self.inputs, self.outputs, self.mem_used_mb, self.runs, self.age_s, ", from the cache" if self.from_cache else "")


class Model:
    """A TFLite model on the NPU.

    Model(model, name=None, device="auto", load_timeout=600, cache_dir=None, max_inflight=2, async_gap_us=300, split=True, split_ratio=4, split_max_error=0,
          cpu_ops=())

    model: path to a .tflite file, or its bytes. The model is identified by the sha256 of these bytes:
      * if it is loaded on the NPU already (by this or another program), the object is attached to it: nothing is loaded and nothing is
        checked; `attached` is True;
      * otherwise it is loaded. Before that, the free memory of the NPU die is compared with what the model needs (the memory measured at an
        earlier load of the same model, kept next to the compiled-graph cache, or an estimate from the file size); a model that does not fit is
        refused with NpuError naming the loaded models, before anything is sent. The first load builds the graph on the NPU (13 s for
        MobileNet V2 uint8, a minute or more for an int8 model with per-channel weights); the compiled graph is kept in cache_dir (default
        ~/.cache/npu-models, environment NPU_CACHE; cache_dir="" switches the cache off) and the next loads take a fraction of a second.
    name: the name in models() (default: the file name).
    device: where it runs.
        "auto" (default): NPU and CPU together - the plan autotune() found for this model if there is one, else integer models on the NPU (with what the
            NPU cannot do on the CPU) and float models on the CPU of the NPU die (faster for them than the float16 path of this NPU).
        "npu": strictly the whole model on the NPU; if an operator would stay on the CPU the load fails with NpuError naming it (use "auto" then).
        "cpu": strictly the whole model on the CPU of the NPU die (TensorFlow Lite with XNNPACK on its two Cortex-A35).
    split: for "npu", split per-channel int8 layers into per-tensor groups (the fast path of the NPU; normally not changed).
    split_ratio: for "npu", the channel scales in one group differ at most this much (default 4; 2 or 1.5: more exact and slower).
    split_max_error: for "npu", > 0: the ratio per layer, the largest of split_ratio, 2, 1.5, 1.25 whose weight error (relative RMS against the int8
        weights) is at most this; a layer that needs less keeps its per-channel weights (exact, slower). autotune() chooses these.
    cpu_ops: internal (the mixed plans of autotune()).
    load_timeout: seconds to wait for the load.
    max_inflight, async_gap_us: see async_exec.

    Attributes: id, name, key, plan, attached, from_cache, inputs and outputs (lists of Tensor), load_ms, mem_used_kb, last_invoke_us.

    close() lets go of the model (it stays loaded and other programs may use it); stop() unloads it from the NPU. As a context manager the object
    is closed at the end of the block, not stopped. Several threads may use one Model; the runs of a model are served one after another.
    """

    def __init__(self, model, name=None, device="auto", load_timeout=600, cache_dir=None, max_inflight=2, async_gap_us=300, split=True, split_ratio=4,
                 split_max_error=0, cpu_ops=()):
        if isinstance(model, (bytes, bytearray, memoryview)):
            src = bytes(model)
        else:
            src = os.fspath(model)
            if not os.path.isfile(src):
                raise NpuError("cannot read %s" % src)
        use_cache = cache_dir != ""
        if device not in ("auto", "npu", "cpu"):
            raise NpuError('device must be "auto", "npu" or "cpu", not %r' % (device,))
        self._m = _core.Model(src, name or "", int(load_timeout), use_cache, cache_dir or "", int(max_inflight), int(async_gap_us),
                              str(device), bool(split), float(split_ratio), float(split_max_error), [int(c) for c in cpu_ops])
        self.inputs = [Tensor(t) for t in self._m.inputs]
        self.outputs = [Tensor(t) for t in self._m.outputs]
        self.id, self.name, self.key = self._m.id, self._m.name, self._m.key
        self.attached, self.from_cache = self._m.attached, self._m.from_cache
        self.load_ms, self.mem_used_kb = self._m.load_ms, self._m.mem_used_kb

    @property
    def plan(self):
        """Where the model runs: "npu", "npu-r<split ratio>", "npu-nosplit", "cpu" or "mixed:<operator codes on the CPU>[:r<ratio>|:nosplit][:from=<node>]"
        (from=N: the operators from index N on run on the CPU; "" when attached to an older load)."""
        return self._m.plan

    def ops(self):
        """The operator types of the model and where they run: a list of OpStat (empty for a model loaded from its compiled graph: all on the NPU)."""
        return [OpStat(o) for o in self._m.ops()]

    def autotune(self, sample=None, tolerance=0.98, runs=5, max_trials=12):
        """Find the fastest way to run this model with the same results, keep it, and switch to it.

        The search, step by step (every candidate is compared with the reference, the whole model on the CPU of the NPU die):
          1. the whole model on the NPU. A model with per-channel int8 layers: split ratio 4; if that is not right, the split ratio per layer from a
             bound on the weight error, the bound by bisection (0.02 ... 0.000625: smaller is more exact and slower); if none is right, no split.
             These loads also time every node of the graph on the CPU (the weights of the cost model);
          2. if none is right: where the result goes wrong. The latest node from which on the CPU the result is right (bisection), then whether a
             window of nodes there is enough; those nodes stay on the CPU;
          3. a cost model predicts the time of any placement of the nodes (NPU or CPU each) from the profile, the cuts between NPU and CPU parts
             and the bytes copied at each cut, fitted to every measurement so far; the cheapest placement is found by dynamic programming over
             the execution order, measured, the model fitted again, until it expects nothing 3 % faster than the best right plan.
        Steps 2 and 3 together try at most max_trials candidates. Every result has predicted_ms: what the model expected (0: not predicted). Each candidate runs the sample (an input array, or a list for several inputs; None: random
        data, but real data is better) and is compared with the reference: the lowest cosine similarity of the outputs must be at least `tolerance`
        (default 0.98: about 20 % relative error of the outputs; 0.99: about 14 %, 0.95: about 32 %).
        Every candidate is loaded and compiled once: for a big model this takes minutes, once.

        The fastest candidate that passes is kept next to the compiled-graph cache (later loads with device "auto" use it, a whole-NPU plan from its cached
        graph) and this object is reloaded with it (programs attached to the old load lose it). Returns the candidates as dicts
        {plan, ms, predicted_ms, cosine, ok, error}, the chosen one first; ms is one exec() with the USB transfer."""
        x = None if sample is None else self._prepare(sample)
        r = self._m.autotune(x, float(tolerance), int(runs), int(max_trials))
        self.inputs = [Tensor(t) for t in self._m.inputs]
        self.outputs = [Tensor(t) for t in self._m.outputs]
        self.id, self.attached, self.from_cache = self._m.id, self._m.attached, self._m.from_cache
        self.load_ms, self.mem_used_kb = self._m.load_ms, self._m.mem_used_kb
        return r

    @property
    def last_invoke_us(self):
        """Microseconds the last run took on the NPU die (without the USB transfer)."""
        return self._m.last_invoke_us

    def _prepare(self, x):
        if isinstance(x, np.ndarray) or not isinstance(x, (list, tuple)):
            x = [x]
        if len(x) != len(self.inputs):
            raise NpuError("the model has %d input(s), %d given" % (len(self.inputs), len(x)))
        out = []
        for a, t in zip(x, self.inputs):
            a = np.asarray(a)
            if a.dtype != t.dtype:
                if np.issubdtype(a.dtype, np.floating) and t.scale and t.dtype in (np.uint8, np.int8, np.int16):
                    a = t.quantize(a)
                else:
                    a = a.astype(t.dtype)
            out.append(np.ascontiguousarray(a))
        return out

    def exec(self, x, timeout=30):
        """Run once and return the outputs: a list of numpy arrays in the dtypes and shapes of the model (outputs[i].dequantize(a) gives float32).

        x: an array, or a list of arrays for a model with several inputs. A float array for an integer input is quantised with the scale of the
        input; another dtype is converted. last_invoke_us is the time on the NPU die afterwards. Raises NpuError (for example when another
        program stopped the model)."""
        return self._m.exec(self._prepare(x), int(timeout))

    def async_exec(self, x, callback):
        """EXPERIMENTAL. Queue a run and return at once; callback(outputs, error) is called from a helper thread when the result is there
        (outputs as exec() returns them and error None, or None and the error text). At most max_inflight runs of this model are on their way
        (the call waits for a free slot).

        A request sent right after a reply while others are in flight (as pipelining does) was seen to hang the USB transfer, and with it the xHCI of
        the board: a pause of async_gap_us (default 300 us) before each request avoids it. exec() is not affected."""
        self._m.async_exec(self._prepare(x), callback)

    def close(self):
        """Let go of the model: this object cannot run it any more; the model stays loaded on the NPU (for this or other programs)."""
        self._m.close()

    def stop(self):
        """Unload the model from the NPU (for every program that uses it) and close this object. Its memory on the NPU die is free again."""
        self._m.stop()

    unload = stop

    @property
    def closed(self):
        """True after close() or stop()."""
        return self._m.closed

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def __repr__(self):
        return "Model(id=%d %r, %s, %d in / %d out%s%s)" % (self.id, self.name, self.plan or "?", len(self.inputs), len(self.outputs),
                                                            ", attached" if self.attached else "", ", closed" if self.closed else "")


NpuModel = Model   # the name of the research prototype


def info():
    """The state of the NPU without loading anything: dict(chip, chip_ok, driver, mem_total_kb, mem_avail_kb, models, proto, via).

    models: the number of loaded models (models() lists them); via: "daemon" or "usb"."""
    return _core.info()


def models():
    """The models loaded on the NPU (by any program): a list of ModelInfo, in the order of their ids."""
    return [ModelInfo(m) for m in _core.models()]


def stop(which):
    """Unload models from the NPU.

    which: a model id (int), a name or a sha256 key (str), a Model or ModelInfo, or "all". Returns the number of models unloaded (0 if nothing
    matched). Programs that still use a stopped model get NpuError on their next run."""
    if isinstance(which, (Model, ModelInfo)):
        return _core.stop_id(which.id)
    if isinstance(which, int):
        return _core.stop_id(which)
    return _core.stop_name(str(which))


def find_devices():
    """The NPU dies on the USB bus: a list of dicts {bus, dev, serial, sysfs}. Empty if the NPU firmware is not loaded."""
    return _core.find_devices()


def is_quantized(model):
    """True if most of the constant data of the model (a path or the .tflite bytes) is int8 / uint8: device "auto" puts such a model on the NPU."""
    data = bytes(model) if isinstance(model, (bytes, bytearray, memoryview)) else open(model, "rb").read()
    return _core.is_quantized(data)
