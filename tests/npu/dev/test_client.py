import sys, time, threading, faulthandler, numpy as np
faulthandler.dump_traceback_later(40, exit=False)   # all threads' stacks if something hangs
sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "..", "..", "python"))
from rk3399pro_npu import NpuModel
t = time.time()
npu = NpuModel(sys.argv[1] if len(sys.argv) > 1 else "/tmp/mobilenet_v2_1.0_224_quant.tflite")
print("loaded in %.1f s (server says %d ms, %d MB)" % (time.time() - t, npu.load_ms, npu.mem_used_kb // 1024))
print("hello:", npu.hello)
print("inputs:", npu.inputs); print("outputs:", npu.outputs)
x = np.random.RandomState(0).randint(0, 255, npu.inputs[0].shape).astype(npu.inputs[0].dtype)
y = npu.exec(x)[0]
print("result: argmax", int(y.argmax()), "invoke on the NPU %d us" % npu.last_invoke_us)
n = 200; t = time.time()
for _ in range(n): npu.exec(x)
dt = (time.time() - t) / n
print("exec, one by one: %.2f ms per frame, %.0f FPS" % (dt * 1000, 1 / dt))
done = threading.Event(); cnt = [0]
def cb(out, err):
    cnt[0] += 1
    if cnt[0] == n: done.set()
t = time.time()
for _ in range(n): npu.async_exec(x, cb)
done.wait(60); dt = (time.time() - t) / n
print("async_exec, %d queued: %.2f ms per frame, %.0f FPS (results %d)" % (n, dt * 1000, 1 / dt, cnt[0]))
try:
    NpuModel(sys.argv[1] if len(sys.argv) > 1 else "/tmp/mobilenet_v2_1.0_224_quant.tflite")
except Exception as e:
    print("second model while the first is loaded ->", type(e).__name__, e)
npu.stop(); print("closed")
