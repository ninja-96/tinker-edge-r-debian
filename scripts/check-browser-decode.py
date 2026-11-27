#!/usr/bin/env python3
"""Check the hardware decoder and displayed H.264 frame contents in one Chromium tab."""
import base64
import contextlib
import functools
import http.server
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import threading
import time

PAGE = """<style>body { margin: 0 } video { width: 320px; height: 180px }</style>
<video autoplay muted loop src="/clip.mp4"></video>"""


def check(directory):
    public = directory / "www"
    public.mkdir()
    public.joinpath("index.html").write_text(PAGE)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i",
                    "color=red:s=1280x720:d=1", "-f", "lavfi", "-i", "color=blue:s=1280x720:d=1",
                    "-filter_complex", "concat=n=2:v=1:a=0", "-c:v", "libx264",
                    "-pix_fmt", "yuv420p", str(public / "clip.mp4")], check=True)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=public)
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        return probe(directory, f"http://127.0.0.1:{server.server_port}/")


def probe(directory, url):
    # Chromium's debugging pipe takes commands on fd 3 and returns NUL-delimited JSON on 4.
    with contextlib.ExitStack() as stack:
        incoming, send = os.pipe()
        receive, outgoing = os.pipe()
        writer = stack.enter_context(os.fdopen(send, "wb", buffering=0))
        reader = stack.enter_context(os.fdopen(receive, "rb", buffering=0))
        # Remap descriptors in a child after pass_fds has closed unrelated descriptors.
        launcher = ("import os,sys; a,b=map(int,sys.argv[1:3]); os.dup2(a,3); "
                    "os.dup2(b,4); os.execvp(sys.argv[3],sys.argv[3:])")
        args = [sys.executable, "-c", launcher, str(incoming), str(outgoing), "chromium",
                "--ozone-platform=wayland", "--remote-debugging-pipe", "--password-store=basic", "--no-first-run",
                f"--user-data-dir={directory}/profile", "--autoplay-policy=no-user-gesture-required"]
        if os.getuid() == 0:
            args.append("--no-sandbox")
        try:
            proc = subprocess.Popen(args + ["about:blank"], pass_fds=(incoming, outgoing))
        finally:
            os.close(incoming)
            os.close(outgoing)
        try:
            return check_tab(reader, writer, url)
        finally:
            with contextlib.suppress(BrokenPipeError):
                writer.write(b'{"id":0,"method":"Browser.close"}\0')
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def check_tab(reader, writer, url):
    deadline = time.monotonic() + 60
    pending = bytearray()
    players = {}
    sequence = 0

    def call(method, params=None, session=None):
        nonlocal sequence
        sequence += 1
        message = dict(id=sequence, method=method, params=params or {})
        if session:
            message["sessionId"] = session
        writer.write(json.dumps(message).encode() + b"\0")
        while True:
            while 0 not in pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([reader], [], [], remaining)[0]:
                    raise TimeoutError(f"{method}: {players}")
                chunk = reader.read(65536)
                if not chunk:
                    raise RuntimeError("Chromium closed its debugging pipe")
                pending.extend(chunk)
            end = pending.index(0)
            msg = json.loads(pending[:end])
            del pending[:end + 1]
            if msg.get("method") == "Media.playerPropertiesChanged" and msg.get("sessionId") == session:
                event = msg["params"]
                players.setdefault(event["playerId"], {}).update(
                    (prop["name"], prop["value"]) for prop in event["properties"])
            if msg.get("id") == sequence:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg["result"]

    gpu = call("SystemInfo.getInfo")["gpu"]["auxAttributes"].get("glRenderer", "")
    if not any(name in gpu.lower() for name in ("panfrost", "mali-t860")):
        raise RuntimeError(f"unexpected GL renderer: {gpu}")
    target = call("Target.createTarget", {"url": "about:blank"})["targetId"]
    session = call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
    call("Media.enable", session=session)
    call("Page.navigate", {"url": url}, session)
    colors = set()
    while time.monotonic() < deadline:
        ready = call("Runtime.evaluate", {"expression":
            "document.querySelector('video')?.readyState >= 2"}, session)
        if ready["result"].get("value") is not True:
            time.sleep(0.25)
            continue
        # Sample the video rectangle from Chromium's composited page, including GPU import.
        image = call("Page.captureScreenshot", {"format": "png", "clip": {
            "x": 150, "y": 80, "width": 4, "height": 4, "scale": 1}}, session)
        pixel = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", "pipe:0", "-vf", "scale=1:1",
                                "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"],
                               input=base64.b64decode(image["data"]), stdout=subprocess.PIPE,
                               timeout=5, check=True).stdout
        r, g, b = pixel
        if r > 180 and g < 70 and b < 70:
            colors.add("red")
        if b > 180 and g < 70 and r < 70:
            colors.add("blue")
        for properties in players.values():
            decoder = properties.get("kVideoDecoderName", "")
            if not decoder or len(colors) < 2 or properties.get("kFrameUrl") != url:
                continue
            if properties.get("kIsPlatformVideoDecoder") == "false":
                raise RuntimeError(f"red and blue frames presented by software decoder {decoder}")
            if properties.get("kIsPlatformVideoDecoder") == "true":
                return f"{gpu}, {decoder}, red and blue frames presented"
        time.sleep(0.25)
    raise RuntimeError(f"hardware decoder and both expected colors required: {players}")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        print(check(Path(tmp)))
