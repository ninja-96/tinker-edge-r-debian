# Converter ONNX to .rknn (RKNN-Toolkit 1.7.5 in Docker)

Rockchip's converter is closed and runs only on Python 3.6-3.8 (x86_64), so it is kept in this image;
the conversion script is inside the image. The Toolkit wheel is not stored in the repository: the build
downloads it from Rockchip's release and checks its md5.

Build once (15-30 minutes, downloads the Toolkit, about 700 MB, and the Python packages; to save the 700 MB put
the release tarball `rknn-toolkit-v1.7.5-packages.tar.gz` next to the `Dockerfile` as `pk.tgz`, its md5 is checked):

    docker build -t rk3399pro-npu-converter docker/rknn-toolkit

Convert (the current directory is mounted as `/work`; paths are relative to it):

    docker run --rm -u $(id -u):$(id -g) -v "$PWD":/work rk3399pro-npu-converter model.onnx -o model.rknn

With 8-bit quantisation (a text file with one image path per line):

    docker run --rm -u $(id -u):$(id -g) -v "$PWD":/work rk3399pro-npu-converter model.onnx -o model.rknn \
        --quantize images.txt --mean 123.675,116.28,103.53 --scale 58.4

All options: `docker run --rm rk3399pro-npu-converter --help`. The model must be ONNX opset 8-12.
Copy `model.rknn` to the board and run it with the `rk3399pro_npu` Python module (docs/NPU.md).

Notes
- The Toolkit is old: the image pins the packages it breaks with (numpy 1.23.5, onnx 1.10.0, networkx 1.11, torch 1.10.0,
  tensorflow-cpu 2.2.0), and everything else is the newest release that supports Python 3.8. The reasons are in the `Dockerfile`.
- Runs on x86_64. There are aarch64 wheels of the Toolkit (Python 3.5 and 3.7), not used here.
- Checked: a small convolution model (opset 12) converted here and run on the board with `rk3399pro_npu`.
