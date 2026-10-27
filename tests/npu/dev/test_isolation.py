import sys, os, time, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "python"))
from rk3399pro_npu import NpuModel, NpuError
order = sys.argv[1:]
x8 = np.random.RandomState(0).randint(0, 255, (1, 224, 224, 3)).astype(np.uint8)
for path in order:
    t = time.time()
    try:
        m = NpuModel(path, cache_dir="/tmp/nbgcache")
    except NpuError as e:
        print("%-32s -> refused after %.1f s: %s" % (os.path.basename(path), time.time() - t, str(e)[:300]), flush=True); continue
    x = x8.astype(m.inputs[0].dtype) if m.inputs[0].dtype in (np.uint8, np.int8) else (x8.astype(np.float32) / 127.5 - 1)
    m.exec(x); n = 100; t2 = time.time()
    for _ in range(n): m.exec(x)
    print("%-32s -> loaded in %.1f s, NPU %.1f ms, exec %.1f ms" % (os.path.basename(path), time.time() - t, m.last_invoke_us / 1000, (time.time() - t2) / n * 1000), flush=True)
    m.close()
print("session over, the NPU is still there")
