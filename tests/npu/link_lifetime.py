"""Run with the built rk3399pro_npu Python module. Uses a local socket in place of USB."""
import gc
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading

import numpy as np
import rk3399pro_npu as npu

HEADER = struct.Struct("<IHHIII")


def receive(conn, size):
    result = bytearray()
    while len(result) < size:
        piece = conn.recv(size - len(result))
        if not piece:
            raise EOFError
        result.extend(piece)
    return result


def check(mode):
    pending, release, completed, stopped = (threading.Event() for _ in range(4))
    unloads = []
    with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_UNIX) as listener:
        os.environ["NPU_SOCKET"] = tmp + "/npu.sock"
        listener.bind(os.environ["NPU_SOCKET"])
        listener.listen()

        def serve():
            for _ in range(2):
                conn, _ = listener.accept()
                with conn:
                    try:
                        while True:
                            magic, op, _, tag, model_id, size = HEADER.unpack(receive(conn, HEADER.size))
                            data = receive(conn, size)
                            if op == 1:  # HELLO
                                reply = struct.pack("<8I32s", 3, 0x8000, 0x7100, 0x45080001, 1, 1000000, 900000, 0, b"test")
                            elif op == 7:  # LIST
                                reply = bytes(4)
                            elif op == 2:  # LOAD with one uint8 input and output
                                tensor = struct.pack("<BBH6ifiI32s", 3, 1, 0, 1, 0, 0, 0, 0, 0, 1., 0, 1, b"x")
                                reply = struct.pack("<5I", 1, 1, 1, 1, 0) + tensor * 2 + bytes(4)
                                model_id = 1
                            elif op == 3:  # RUN
                                pending.set()
                                assert release.wait(10)
                                reply = struct.pack("<I", 1) + data
                            elif op == 4:  # UNLOAD
                                unloads.append(model_id)
                                reply = b""
                            else:
                                raise AssertionError(op)
                            conn.sendall(HEADER.pack(magic, op, 0, tag, model_id, len(reply)) + reply)
                    except (EOFError, BrokenPipeError, ConnectionResetError):
                        stopped.set()

        server = threading.Thread(target=serve, daemon=True)
        server.start()
        models = [npu.Model(b"fixture", device="cpu", cache_dir="")]

        def done(outputs, error):
            if mode == "callback":
                assert not error and outputs[0].item() == 1
                try:
                    npu.models()
                except npu.NpuError as exc:
                    assert "reply thread" in str(exc)
                else:
                    raise AssertionError("synchronous request from the callback succeeded")
                models.clear()
                gc.collect()
            completed.set()

        models[0].async_exec(np.array([1], np.uint8), done)
        assert pending.wait(10)
        if mode == "close":
            models[0].close()
            models[0].close()
        elif mode == "with":
            with models[0]:
                pass
        elif mode == "del":
            models.clear()
            gc.collect()
        release.set()
        assert completed.wait(10) and stopped.wait(10) and not unloads
        npu.Model(b"fixture", device="cpu", cache_dir="").stop()
        server.join(10)
        assert not server.is_alive() and unloads == [1]


if __name__ == "__main__":
    if len(sys.argv) == 2:
        check(sys.argv[1])
    else:
        for mode in ("close", "with", "del", "callback"):
            subprocess.run([sys.executable, __file__, mode], timeout=45, check=True)
            print(mode, "ok", flush=True)
