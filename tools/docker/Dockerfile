# Linux x86-64 development container: the same toolchain as CI (GCC 13,
# Clang 18, clang-tidy 18, sanitizer runtimes, libxdp/libbpf).
#
#   docker build --platform linux/amd64 -t itch-dev tools/docker
#   docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -v itch-build:/b itch-dev \
#     bash -c 'CC=gcc-13 cmake -S /src -B /b/gcc -G Ninja -DCMAKE_CXX_COMPILER=g++-13 \
#              -DITCH_MARCH=x86-64-v3 && cmake --build /b/gcc && ctest --test-dir /b/gcc'
#
# On Apple Silicon this runs under Rosetta for Linux (Docker Desktop setting
# "Use Rosetta for x86_64/amd64 emulation"). What works there:
#   GCC/Clang builds and tests, ASan + UBSan, clang-tidy.
# What does not:
#   TSan (Rosetta's address-space layout is incompatible with TSan's shadow
#   memory), AVX-512 (Rosetta executes up to AVX2), meaningful timings.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        g++-13 clang-18 clang-tidy-18 lld-18 llvm-18 libclang-rt-18-dev \
        cmake ninja-build git ca-certificates python3 \
        libxdp-dev libbpf-dev linux-libc-dev pkg-config iproute2 iputils-ping && \
    rm -rf /var/lib/apt/lists/*
