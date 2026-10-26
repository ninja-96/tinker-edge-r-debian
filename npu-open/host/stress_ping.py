import sys, time, threading, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from npu_client import NpuModel, NpuLink, T_PING
class Link(NpuModel):
    def __init__(self): NpuLink.__init__(self, 2)
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40
size = int(sys.argv[2]) if len(sys.argv) > 2 else 150528
nthreads = int(sys.argv[3]) if len(sys.argv) > 3 else 2
l = Link(); print("hello:", l._hello()["driver"])
payload = bytes((i * 7) & 255 for i in range(size)); cnt = [0]; stop = time.time() + secs; err = []
def worker():
    try:
        while time.time() < stop:
            r = l._call(T_PING, payload, timeout=10)
            if r != payload: err.append("echo differs"); return
            cnt[0] += 1
    except Exception as e:
        err.append(repr(e))
ts = [threading.Thread(target=worker) for _ in range(nthreads)]
t0 = time.time(); [t.start() for t in ts]
last = 0
while any(t.is_alive() for t in ts):
    time.sleep(1); print("%4.0f s: %d round trips, %.1f MB/s each way %s" % (time.time() - t0, cnt[0], cnt[0] * size / (time.time() - t0) / 1e6, err), flush=True)
[t.join() for t in ts]
print("done:", cnt[0], "round trips of", size, "bytes with", nthreads, "threads;", "errors:", err)
l._closed = True
