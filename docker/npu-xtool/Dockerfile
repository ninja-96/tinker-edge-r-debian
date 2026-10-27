# Cross toolchain for the userland of the NPU die (aarch64, glibc 2.29 and libstdc++ 6.0.22 in Rockchip's firmware).
# Debian 11 "bullseye": glibc 2.31 headers (what ordinary code references stays at versions <= 2.29), gcc 10, cmake 3.18.
# Not newer: Debian 12 and later reference GLIBC_2.34 (libpthread merged into libc), which the NPU's glibc 2.29 lacks.
# Not Alpine: musl binaries cannot load the glibc libraries of the Vivante driver.
FROM debian:bullseye-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN printf '%s\n' 'deb http://archive.debian.org/debian bullseye main' 'deb http://archive.debian.org/debian bullseye-updates main' 'deb http://archive.debian.org/debian-security bullseye-security main' > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Retries=8 -o Acquire::http::Timeout=60 update \
    && apt-get -o Acquire::Retries=8 -o Acquire::http::Timeout=60 install -y --no-install-recommends build-essential cmake git ca-certificates python3 \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*
