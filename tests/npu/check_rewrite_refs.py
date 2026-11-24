# Checks the seven rewrite-op models of npu-selftest against their CPU references in expected.npz
# without a board: each model is run with TFLite on the CPU and compared with the stored
# reference, the same comparison the board self-test makes. A stale or edited reference fails here.
#   docker run --rm -v $PWD/tests/npu:/w --entrypoint python npu-tf213 /w/check_rewrite_refs.py [npz]
import sys
import numpy as np
import tensorflow as tf

names = ("slice", "tile", "ceil", "cos", "round", "greater", "select")
exp = np.load(sys.argv[1] if len(sys.argv) > 1 else "/w/expected.npz")
fails = 0
for name in names:
    it = tf.lite.Interpreter(model_content=open("/w/%s.tflite" % name, "rb").read())
    it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    it.set_tensor(i["index"], exp["in_" + name].astype(i["dtype"]))
    it.invoke()
    out, ref = it.get_tensor(o["index"]), exp["out_" + name]
    diff = float(np.abs(out.astype(np.float32) - ref.astype(np.float32)).max())
    tol = 3 if ref.dtype == np.int8 else 1e-3
    ok = diff <= tol
    print("%-10s %s  max difference %g (tolerance %g)" % (name, "ok  " if ok else "FAIL", diff, tol))
    fails += not ok
print("OK: all %d references match" % len(names) if not fails else "FAIL: %d reference(s) do not match" % fails)
sys.exit(1 if fails else 0)
