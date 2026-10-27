import sys, time, threading, faulthandler, os, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "python"))
faulthandler.dump_traceback_later(60, exit=True)
from rk3399pro_npu import NpuModel
inflight = int(sys.argv[1]); mode = sys.argv[2]; n = int(sys.argv[3])
npu = NpuModel("/tmp/mobilenet_v2_1.0_224_quant.tflite", max_inflight=inflight)
x = np.random.RandomState(0).randint(0, 255, npu.inputs[0].shape).astype(npu.inputs[0].dtype)
t = time.time()
if mode == "exec":
    for _ in range(n): npu.exec(x)
else:
    done = threading.Event(); cnt = [0]
    def cb(o, e):
        cnt[0] += 1
        if cnt[0] == n: done.set()
    for _ in range(n): npu.async_exec(x, cb)
    done.wait(40)
dt = (time.time() - t) / n
print("inflight %d %s x%d: %.2f ms per frame, %.0f FPS" % (inflight, mode, n, dt * 1000, 1 / dt))
npu.stop()
