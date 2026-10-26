# Converter ONNX -> .rknn for the RK3399Pro NPU: Rockchip's closed RKNN-Toolkit 1.7.5 needs
# Python 3.6-3.8 (it is compiled Python), so it lives in this image and nothing else does.
# The Toolkit wheel is NOT stored in the repository: it is downloaded from Rockchip's release at build
# time and its md5 is checked (if the release tarball pk.tgz lies next to this file it is used instead).
# Build:  docker build -t rk3399pro-npu-converter docker/rknn-toolkit      (x86_64 hosts)
FROM python:3.8-slim-bullseye

ENV PIP_NO_CACHE_DIR=1 PIP_DISABLE_PIP_VERSION_CHECK=1 PIP_DEFAULT_TIMEOUT=120 PIP_RETRIES=15
ARG TOOLKIT_URL=https://github.com/rockchip-linux/rknn-toolkit/releases/download/v1.7.5/rknn-toolkit-v1.7.5-packages.tar.gz
ARG TARBALL_MD5=0818a331d3bba755036cfee296bdad31
ARG WHEEL=rknn_toolkit-1.7.5-cp38-cp38-linux_x86_64.whl
ARG WHEEL_MD5=da4cd1a1968f9e9aa5414a67598f5374

# Packages: the newest that work with the Toolkit; these are pinned because it breaks with newer ones (found by trying):
#  - tensorflow-cpu 2.2.0 (the Toolkit imports TensorFlow even for ONNX; Rockchip's list for Python 3.8) with
#    protobuf 3.20.3, and numpy 1.23.5 (TensorFlow 2.2 uses np.object, removed in numpy 1.24)
#  - torch 1.10.0 CPU (imported by the optimiser; torch 2.x fails with the Toolkit's logging setup)
#  - onnx 1.10.0 (the Toolkit calls onnx._serialize), networkx 1.11 (it uses networkx.utils.make_str)
RUN pip install --upgrade pip setuptools wheel \
    && pip install "tensorflow-cpu==2.2.0" "protobuf==3.20.3" "numpy==1.23.5" "onnx==1.10.0" "networkx==1.11" \
        "torch==1.10.0+cpu" --extra-index-url https://download.pytorch.org/whl/cpu
RUN pip install onnxoptimizer onnxruntime opencv-python-headless Jinja2 flask flatbuffers matplotlib ply psutil \
        requests ruamel.yaml scipy tqdm scikit-learn

COPY convert.py pk.tg[z] /tmp/ctx/
RUN mkdir /tk && cd /tk \
    && if [ -f /tmp/ctx/pk.tgz ]; then mv /tmp/ctx/pk.tgz pk.tgz; \
       else python -c "import sys,urllib.request as u; u.urlretrieve(sys.argv[1], 'pk.tgz')" "$TOOLKIT_URL"; fi \
    && echo "$TARBALL_MD5  pk.tgz" | md5sum -c - \
    && tar xzf pk.tgz "packages/$WHEEL" && echo "$WHEEL_MD5  packages/$WHEEL" | md5sum -c - \
    && mv "packages/$WHEEL" /tk/ && pip install --no-deps /tk/$WHEEL && rm -rf /tk /tmp/ctx/pk.tgz

RUN cp /tmp/ctx/convert.py /usr/local/bin/convert.py && rm -rf /tmp/ctx
WORKDIR /work
ENTRYPOINT ["python", "/usr/local/bin/convert.py"]
