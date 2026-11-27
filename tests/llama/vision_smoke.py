"""Send a solid-red PNG to a running llama-server and check its answer."""
import argparse
import base64
import json
import struct
import urllib.request
import zlib


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    args = parser.parse_args()
    width = 128
    header = struct.pack(">2I5B", width, width, 8, 2, 0, 0, 0)
    pixels = (b"\0" + b"\xff\0\0" * width) * width
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
           + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
    content = [
        {"type": "image_url", "image_url": {
            "url": "data:image/png;base64," + base64.b64encode(png).decode()}},
        {"type": "text", "text": "What color is the square? Answer with one word."},
    ]
    payload = {"messages": [{"role": "user", "content": content}],
               "chat_template_kwargs": {"enable_thinking": False},
               "temperature": 0, "max_tokens": 16, "seed": 1}
    req = urllib.request.Request(args.url.rstrip("/") + "/v1/chat/completions",
                                 data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=1800) as response:
        result = json.load(response)
    answer = result["choices"][0]["message"]["content"]
    print(json.dumps({"answer": answer, "usage": result["usage"]}))
    if answer.strip().lower().strip(".!") != "red":
        raise SystemExit("Expected a one-word answer: red")


if __name__ == "__main__":
    main()
