#!/usr/bin/env bash
# Build the board side of the NPU stack (aarch64, Debian 13): the daemon npu-daemon, the C++ library librk3399pro_npu and the Python module
# rk3399pro_npu._core (pybind11, for the Python 3.13 of the image). Sources: npu-open/host/. Run in the builder container:
#   ./run.sh scripts/build-npu-host.sh        -> out/npu-host/{npu-daemon,librk3399pro_npu.so.0,rk3399pro_npu.hpp,_core.cpython-313-aarch64-linux-gnu.so}
# Optimisation: -O3 -mcpu=cortex-a72.cortex-a53 (the CPU of the board).
set -euo pipefail
cd "$(dirname "$0")/.."
S=npu-open/host
O=out/npu-host
CXX=${CXX:-aarch64-linux-gnu-g++}
PY=python3.13
EXT=.cpython-313-aarch64-linux-gnu.so
[ -f /usr/include/$PY/Python.h ] && [ -f /usr/include/aarch64-linux-gnu/$PY/pyconfig.h ] || { echo "libpython3.13-dev:arm64 missing (docker/Dockerfile)" >&2; exit 1; }
[ -f /usr/include/pybind11/pybind11.h ] || { echo "pybind11-dev missing (docker/Dockerfile)" >&2; exit 1; }
mkdir -p "$O"
F=(-std=c++17 -O3 -mcpu=cortex-a72.cortex-a53 -fPIC -Wall -Wextra -pthread -I"$S" -Inpu-open/server)
$CXX "${F[@]}" -shared -Wl,-soname,librk3399pro_npu.so.0 -o "$O/librk3399pro_npu.so.0" "$S/rk3399pro_npu.cc"
$CXX "${F[@]}" -o "$O/npu-daemon" "$S/npu_daemon.cc"
# the module carries its own copy of the library: no search path for the shared one is needed
$CXX "${F[@]}" -shared -fvisibility=hidden -I/usr/include/$PY -I/usr/include/aarch64-linux-gnu/$PY -o "$O/_core$EXT" "$S/python_core.cc" "$S/rk3399pro_npu.cc"
aarch64-linux-gnu-strip --strip-unneeded "$O/librk3399pro_npu.so.0" "$O/_core$EXT"
aarch64-linux-gnu-strip "$O/npu-daemon"
cp "$S/rk3399pro_npu.hpp" "$O/"
# the module contains pybind11 (header-only, BSD-3-Clause, from Debian): its licence goes into the image
mkdir -p "$O/licenses" && cp /usr/share/doc/pybind11-dev/copyright "$O/licenses/pybind11-copyright"
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$O"
ls -l "$O"
