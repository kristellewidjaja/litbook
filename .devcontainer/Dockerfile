# litbook development environment: Linux toolchain for a macOS (or any) host.
# Ubuntu 26.04 LTS ships GCC 15 and Clang 21 as its defaults.
FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc g++ \
        clang lld llvm libclang-rt-dev \
        clang-format clang-tidy clang-tools \
        cmake ninja-build \
        gdb \
        git ca-certificates \
        python3 \
        linux-perf \
    && rm -rf /var/lib/apt/lists/*
# llvm provides llvm-symbolizer, so sanitizer stack traces show file:line.
# libclang-rt-dev holds Clang's sanitizer and libFuzzer runtimes.
# linux-perf provides perf. On Ubuntu 26.04 it is a single binary that is not tied to
# one kernel version, so it works with Docker Desktop's own kernel. That VM exposes no
# hardware counters, so profile with software events (perf record -e cpu-clock).

# The repo is bind-mounted from the host, so its files are owned by a different
# user than the container's; tell git that is expected.
RUN git config --system --add safe.directory /work

WORKDIR /work
CMD ["bash"]
