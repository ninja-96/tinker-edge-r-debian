"""Compare uncached strict NPU execution with LiteRT CPU on identical inputs on the board."""
import argparse
import sys
from time import perf_counter_ns

import numpy as np
import rk3399pro_npu as npu


def exercise(path, device, batches, split):
    model = npu.Model(path, device=device, cache_dir="", split=split)
    if model.attached:
        model.close()
        raise RuntimeError("model already in use")
    try:
        ops = model.ops()
        if device == "npu" and (not ops or any(o.total != o.on_npu for o in ops)):
            raise RuntimeError(f"incomplete NPU coverage: {ops}")
        if batches is None:
            rng = np.random.default_rng(7)
            batches = []
            for index in range(8):
                inputs = []
                for t in model.inputs:
                    if t.dtype == np.float32:  # Quantized backbones may have float boundaries.
                        inputs.append(rng.uniform(-1, 1, size=t.shape).astype(np.float32))
                        continue
                    if t.dtype not in (np.int8, np.uint8):
                        raise ValueError(f"unsupported test input {t.dtype}")
                    codes = np.arange(np.prod(t.shape)) if index == 0 else rng.integers(256, size=t.shape)
                    inputs.append(codes.astype(t.dtype).reshape(t.shape))
                batches.append(inputs)
        model.exec(batches[0])
        outputs, times = [], []
        for inputs in batches:
            start = perf_counter_ns()
            result = model.exec(inputs)
            times.append((perf_counter_ns() - start) / 1e6)
            outputs.append([x.copy() for x in result])
        return batches, outputs, np.median(times)
    finally:
        model.stop()


def compare(path, split):
    batches, expected, cpu_ms = exercise(path, "cpu", None, split)
    _, actual, npu_ms = exercise(path, "npu", batches, split)
    different = 0
    worst = 0
    for batch, ref_batch in zip(actual, expected, strict=True):
        for out, ref in zip(batch, ref_batch, strict=True):
            if out.dtype != ref.dtype or out.shape != ref.shape:
                raise ValueError("output dtype or shape changed")
            different += np.count_nonzero(out != ref)
            worst = max(worst, np.max(np.abs(out.astype(np.float64) - ref)))
    print(f"{path}: CPU {cpu_ms:.2f} ms, NPU {npu_ms:.2f} ms, differing elements {different}, max error {worst:g}", flush=True)
    return different == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("models", nargs="+")
    parser.add_argument("--no-split", action="store_true", help="preserve native per-channel weights")
    args = parser.parse_args()
    ok = True
    for path in args.models:
        try:
            ok = compare(path, not args.no_split) and ok
        except (RuntimeError, ValueError, npu.NpuError) as error:
            print(f"{path}: {error}", flush=True)
            ok = False
    sys.exit(0 if ok else 1)
