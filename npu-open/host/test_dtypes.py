import sys, time, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from npu_client import NpuModel
from PIL import Image
img = np.asarray(Image.open("/tmp/dog_224x224.jpg").convert("RGB").resize((224, 224)), np.float32)
x = img / 127.5 - 1.0                      # the normalisation of these MobileNets: [-1, 1]
x = x[None]
names = sys.argv[1:]
ref = None
for path in names:
    t = time.time()
    try:
        m = NpuModel(path, cache_dir="/tmp/nbgcache")
    except Exception as e:
        print("%-34s LOAD FAILED: %s" % (os.path.basename(path), str(e)[:150])); continue
    lt = time.time() - t
    i, o = m.inputs[0], m.outputs[0]
    try:
        out = m.exec(x)[0].astype(np.float32).ravel()
        if o.scale: out = (out - o.zero_point) * o.scale
        top = out.argsort()[-5:][::-1].tolist()
        n = 200; t = time.time()
        for _ in range(n): m.exec(x)
        ms = (time.time() - t) / n * 1000
        if ref is None: ref = out; refname = os.path.basename(path)
        cos = float(np.dot(out, ref) / (np.linalg.norm(out) * np.linalg.norm(ref) + 1e-9))
        print("%-34s in %s out %s | load %5.1f s (%d MB) | NPU %5.1f ms, exec %6.1f ms | top5 %s | cos to first %.3f"
              % (os.path.basename(path), i.dtype, o.dtype, lt, m.mem_used_kb // 1024, m.last_invoke_us / 1000, ms, top, cos), flush=True)
    except Exception as e:
        print("%-34s RUN FAILED: %s" % (os.path.basename(path), str(e)[:150]))
    m.close()
