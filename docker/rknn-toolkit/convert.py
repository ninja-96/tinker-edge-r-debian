"""Runs inside the container: ONNX -> .rknn with RKNN-Toolkit 1.7.5 (see tools/rk3399pro_npu-convert)."""
import argparse
import sys

from rknn.api import RKNN


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--target", default="rk3399pro")
    ap.add_argument("--quantize", metavar="DATASET", help="text file with one image path per line: 8-bit quantisation")
    ap.add_argument("--mean", default="0,0,0", help="mean value per channel, e.g. 123.675,116.28,103.53")
    ap.add_argument("--scale", default="1", help="one divisor for all channels, e.g. 58.4; input = (pixel - mean) / scale")
    ap.add_argument("--reorder", default="0 1 2", help="channel order, '2 1 0' swaps RGB and BGR")
    ap.add_argument("--inputs", help="names of the model inputs, comma separated (if the converter cannot tell)")
    ap.add_argument("--input-sizes", help="shape(s) w/o batch, e.g. 3,224,224 (several: semicolon)")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    mean = [float(x) for x in a.mean.split(",")]
    if len(mean) != 3:
        sys.exit("--mean needs three values")
    rknn = RKNN(verbose=a.verbose)
    n = len(a.input_sizes.split(";")) if a.input_sizes else 1
    rknn.config(mean_values=[mean] * n, std_values=[[float(a.scale)] * 3] * n, reorder_channel=a.reorder,
                target_platform=[a.target])
    kw = {}
    if a.inputs:
        kw["inputs"] = a.inputs.split(",")
    if a.input_sizes:
        kw["input_size_list"] = [[int(x) for x in s.split(",")] for s in a.input_sizes.split(";")]
    if rknn.load_onnx(model=a.model, **kw) != 0:
        sys.exit("load_onnx failed (ONNX opset 8-12 only, see the message above)")
    # dataset=None passed explicitly makes the Toolkit fail with an AssertionError: leave the argument out
    build_args = dict(do_quantization=True, dataset=a.quantize) if a.quantize else dict(do_quantization=False)
    if rknn.build(**build_args) != 0:
        sys.exit("build failed")
    if rknn.export_rknn(a.output) != 0:
        sys.exit("export failed")
    rknn.release()
    print("wrote", a.output)


main()
