#!/bin/sh
# Runs Model.autotune() of rk3399pro_npu on models and writes everything the analysis of autotune needs into one log: the system, every candidate
# (plan, measured time, the time the cost model predicted, cosine to the CPU result), the time each autotune took, and the logs of the NPU engines
# (per load: the operators rewritten, the per-channel layers split / kept, the profiled CPU run; and their stderr). Run it on the board; it takes about an hour for the
# default models (every candidate is compiled once on the NPU die: seconds to two minutes each). Nothing is kept: every model gets an empty cache.
#
#   tests/npu/autotune-log.sh [-o file.log] [-d models_dir] [model.tflite:tolerance ...]
#
# Default models (from models_dir, default /tmp/litert): the LiteRT community int8 models of docs/NPU.md, each with a tolerance just above what it
# reaches on the NPU with split ratio 4, so that every step of autotune runs:
#   squeezenet1_1_int8_channelwise.tflite:0.9995 resnet18_int8_channelwise.tflite:0.995
#   mobilenet_v3_large_int8_channelwise.tflite:0.98 efficientnet_b0_int8_channelwise.tflite:0.97
# Needs: python3 with rk3399pro_npu (the image has it; PYTHONPATH is passed on) and adb to the NPU die (for its engine logs; skipped without it).
set -eu
out="autotune-$(date +%Y%m%d-%H%M%S).log"
dir=/tmp/litert
while getopts o:d:h opt; do
	case $opt in
	o) out=$OPTARG ;;
	d) dir=$OPTARG ;;
	*) sed -n '2,15p' "$0"; exit 0 ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -eq 0 ]; then
	set -- squeezenet1_1_int8_channelwise.tflite:0.9995 resnet18_int8_channelwise.tflite:0.995 \
	       mobilenet_v3_large_int8_channelwise.tflite:0.98 efficientnet_b0_int8_channelwise.tflite:0.97
fi
have_adb=0
if command -v adb >/dev/null 2>&1 && adb get-state >/dev/null 2>&1; then have_adb=1; fi
echo "writing $out"
{
	echo "==== system"
	date -u +%Y-%m-%dT%H:%M:%SZ
	uname -a
	python3 -c 'import rk3399pro_npu as n; i = n.info(); print("npu:", {k: i[k] for k in sorted(i)})' 2>&1 || true
	echo "adb to the NPU die: $have_adb"
	if [ $have_adb = 1 ]; then adb shell 'rm -f /tmp/npu_engines.log; : > /tmp/npu_server.log' >/dev/null 2>&1 || true; fi
	echo "==== autotune"
	python3 - "$dir" "$@" <<'PY' 2>&1
import os, sys, time, tempfile, numpy as np, rk3399pro_npu as npu
d = sys.argv[1]
rng = np.random.default_rng(1)
for arg in sys.argv[2:]:
    f, tol = arg.rsplit(":", 1)
    tol = float(tol)
    path = f if os.path.isabs(f) else os.path.join(d, f)
    print("== %s tolerance %s" % (os.path.basename(path), tol), flush=True)
    if not os.path.isfile(path):
        print("   missing: %s" % path, flush=True)
        continue
    try:
        m = npu.Model(path, cache_dir=tempfile.mkdtemp(prefix="autotune-log-"))
        print("   inputs: %s" % [(t.shape, t.dtype.__name__ if hasattr(t.dtype, "__name__") else str(t.dtype)) for t in m.inputs], flush=True)
        xs = []
        for t in m.inputs:   # random data in the range of each input (the same seed every time)
            if np.dtype(t.dtype) == np.int8: xs.append(rng.integers(-128, 128, t.shape, dtype=np.int8))
            elif np.dtype(t.dtype) == np.uint8: xs.append(rng.integers(0, 256, t.shape, dtype=np.uint8))
            else: xs.append(rng.random(t.shape, dtype=np.float32))
        t0 = time.time()
        r = m.autotune(xs if len(xs) > 1 else xs[0], tolerance=tol)
        print("   autotune %.0f s, %d candidates, chosen %s" % (time.time() - t0, len(r), r[0]["plan"]), flush=True)
        print("   %-60s %10s %10s %8s %s" % ("plan", "ms", "predicted", "cosine", "ok / error"), flush=True)
        for c in r:
            print("   %-60s %10.1f %10.1f %8.4f %s" % (c["plan"], c["ms"], c["predicted_ms"], c["cosine"], "ok" if c["ok"] else ("-- " + c["error"])[:200]), flush=True)
        m.stop()
    except Exception as e:
        print("   failed: %s" % e, flush=True)
PY
	echo "==== log of the NPU server and its engines (per load: rewrites, split / kept per-channel layers, profiled CPU run, compile time)"
	if [ $have_adb = 1 ]; then adb shell 'cat /tmp/npu_server.log' 2>&1 || true; else echo "(no adb)"; fi
	echo "==== stderr of the engines (TIM-VX, the VX delegate, TFLite)"
	if [ $have_adb = 1 ]; then adb shell 'cat /tmp/npu_engines.log' 2>&1 || true; else echo "(no adb)"; fi
	echo "==== end"
	date -u +%Y-%m-%dT%H:%M:%SZ
} > "$out" 2>&1
echo "done: $out"
