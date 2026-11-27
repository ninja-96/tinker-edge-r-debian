"""Generate isolated INT8/UINT8 LiteRT fixtures with flatc and LiteRT's schema.fbs."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def make_model(op, code, dtype):
    zero = 5 if dtype == "INT8" else 133
    quant = {"scale": [0.125], "zero_point": [zero]}
    x = dict(name="input", type=dtype, shape=[1, 8, 8, 4], buffer=0, quantization=quant)
    y = dict(x, name="output")
    tensors = [x, y]
    inputs = [0]
    buffers = [{}]
    version = 1
    if op == "DEQUANTIZE":
        del y["quantization"]
        y["type"] = "FLOAT32"
        version = 2 if dtype == "INT8" else 1
    elif op == "QUANTIZE":
        y["quantization"] = dict(quant, scale=[0.25])
    elif op == "LOGISTIC":
        y["quantization"] = dict(scale=[1 / 256], zero_point=[-128 if dtype == "INT8" else 0])
        version = 2 if dtype == "INT8" else 1
    elif op == "MUL":
        tensors.append(dict(x, name="factor"))
        inputs.append(2)
        version = 2 if dtype == "INT8" else 1
    elif op == "PADV2":
        y["shape"] = [1, 9, 11, 4]
        buffers.append(dict(data=list(struct.pack("<8i", 0, 0, 0, 1, 1, 2, 0, 0))))
        buffers.append(dict(data=[zero]))
        tensors.extend([dict(name="pads", type="INT32", shape=[4, 2], buffer=1),
                        dict(x, name="fill", shape=[], buffer=2)])
        inputs.extend([2, 3])
        version = 2 if dtype == "INT8" else 1
    elif op == "BROADCAST_TO":
        x["shape"] = [1, 8, 1, 4]
        buffers.append(dict(data=list(struct.pack("<4i", *y["shape"]))))
        tensors.append(dict(name="shape", type="INT32", shape=[4], buffer=1))
        inputs.append(2)
        version = 3 if dtype == "INT8" else 2
    operator = dict(opcode_index=0, inputs=inputs, outputs=[1])
    if op == "MUL":
        operator.update(builtin_options_type="MulOptions", builtin_options={})
    graph = dict(tensors=tensors, inputs=inputs if op == "MUL" else [0], outputs=[1], operators=[operator])
    return dict(version=3, operator_codes=[dict(builtin_code=op, deprecated_builtin_code=min(code, 127),
                                                version=version)], subgraphs=[graph], buffers=buffers)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flatc")
    parser.add_argument("schema")
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, code in (("DEQUANTIZE", 6), ("QUANTIZE", 114), ("RELU_N1_TO_1", 20), ("BROADCAST_TO", 130),
                           ("PADV2", 60), ("HARD_SWISH", 117), ("LOGISTIC", 14), ("MUL", 18)):
            for dtype in ("INT8", "UINT8"):
                model = make_model(name, code, dtype)
                cases = [("", None)]
                if name == "QUANTIZE":
                    cases.extend((f"_zp{a}_{b}", (a, b)) for a, b in ((0, 255), (255, 0), (31, 192)))
                for suffix, zero_points in cases:
                    if zero_points:
                        low = -128 if dtype == "INT8" else 0
                        for i, offset in enumerate(zero_points):
                            tensor = model["subgraphs"][0]["tensors"][i]
                            tensor["quantization"] = dict(scale=[0.071234 * (i + 1)], zero_point=[low + offset])
                    path = Path(tmp) / f"{name}_{dtype}{suffix}.json"
                    path.write_text(json.dumps(model))
                    subprocess.run([args.flatc, "-b", "-o", str(args.out), args.schema, str(path)], check=True)
