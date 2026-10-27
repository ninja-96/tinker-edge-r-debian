FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
        bc bison flex libssl-dev libelf-dev libncurses-dev \
        device-tree-compiler swig python3 python3-dev python3-setuptools \
        python3-pyelftools python3-cryptography libgnutls28-dev uuid-dev \
        git curl ca-certificates xz-utils bzip2 zstd lz4 cpio rsync kmod file \
        u-boot-tools fdisk gdisk parted dosfstools e2fsprogs mtools \
        mmdebstrap qemu-user-static binfmt-support arch-test debian-archive-keyring \
        qemu-system-arm fakeroot cmake pkg-config gpiod libusb-1.0-0-dev libudev-dev autoconf automake \
        libtool ccache sudo less vim-tiny \
    && rm -rf /var/lib/apt/lists/*

# the Python module of the NPU library (scripts/build-npu-host.sh): the headers of the board's Python 3.13 and pybind11, from Debian
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
        libpython3.13-dev:arm64 pybind11-dev \
    && rm -rf /var/lib/apt/lists/*

# Cortex-M0 toolchain: TF-A builds the RK3399 PMU firmware with it
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi libnewlib-arm-none-eabi \
    && rm -rf /var/lib/apt/lists/*

ENV ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
WORKDIR /work
