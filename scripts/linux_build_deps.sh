#!/usr/bin/env bash
# Installs the Linux build prerequisites inside the Rocky Linux 8 build
# container (spec decision 5: nvidia/cuda:12.9.1-devel-rockylinux8, glibc
# 2.28). CI (.github/workflows/linux.yml) and the local container recipe in
# BUILD.md both run this, so the two can't drift.
#
#   gcc-toolset-13  GCC 13 (C++17, supported by nvcc 12.9) with libstdc++.a for
#                   -static-libstdc++; binaries still need only glibc 2.28
#   cmake           3.26 from AppStream (>= 3.24 for ZLIB_USE_STATIC_LIBS)
#   zlib-static     libz.a (PowerTools), linked into the plugin (decision 6)
#
# Activate the toolset afterwards with:  source /opt/rh/gcc-toolset-13/enable
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "linux_build_deps.sh: run as root (inside the build container)" >&2
    exit 1
fi

dnf -y install dnf-plugins-core
dnf config-manager --set-enabled powertools
dnf -y install \
    gcc-toolset-13-gcc \
    gcc-toolset-13-gcc-c++ \
    gcc-toolset-13-binutils \
    cmake \
    make \
    zlib-devel \
    zlib-static
dnf clean all
