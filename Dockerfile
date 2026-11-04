# librbipc CI-matching build environment.
#
# This image mirrors the GitHub Actions `ubuntu-latest` toolchain (Ubuntu 24.04,
# gcc 13.3.0) so you can reproduce CI builds/tests locally. It deliberately runs
# as the invoking user (see scripts/docker_build.sh) so that build artifacts in
# build/, lib/ and bin/ are NOT owned by root — the classic footgun of a plain
# `docker run -v $PWD:/work` that leaves the workspace un-rebuildable without a
# `sudo chown -R` afterwards.
#
# Build:
#   docker build -t librbipc-ci .
#
# Run (prefer the wrapper script, which maps the current uid/gid):
#   ./scripts/docker_build.sh make asan
#   ./scripts/docker_build.sh make test

FROM ubuntu:24.04

# Non-interactive apt to avoid debconf prompts.
ENV DEBIAN_FRONTEND=noninteractive

# Build/test toolchain. g++ is required even for C builds because the test
# suite includes test_unit_cpp_compat.cpp (linked by the Makefile's C++ rule).
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        g++ \
        make \
        pkg-config \
        clang-tidy \
        gcovr \
        valgrind \
        python3 \
        doxygen \
        bc \
        util-linux \
    && rm -rf /var/lib/apt/lists/*

# The workspace is mounted at /work by scripts/docker_build.sh. No source is
# baked into the image so the same image works for any checkout.
WORKDIR /work

# Default command mirrors the CI "Build All Targets" step.
CMD ["make", "clean", "all"]