# SPDX-License-Identifier: GPL-2.0-or-later
#
# Development image for zigbee2supla: everything needed to build, test,
# format and debug the project, plus the tools used for manual tests
# (mosquitto, python3 for tools/fake_supla_server.py).
#
# Same base as the CI runner (Ubuntu 24.04), so clang-format and the library
# versions match .github/workflows/ci.yml. The image for running the gateway
# is the Dockerfile in the repository root.
#
# Usage: tools/dev/dev.sh <command>
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang-format \
        cmake \
        gdb \
        git \
        libgtest-dev \
        libmosquitto-dev \
        libssl-dev \
        libyaml-cpp-dev \
        mosquitto \
        mosquitto-clients \
        ninja-build \
        nlohmann-json3-dev \
        openssl \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# The container runs with the host user's UID, which has no passwd entry.
ENV HOME=/tmp
# git refuses to work in a repository owned by another user otherwise.
RUN git config --system --add safe.directory '*'

CMD ["/bin/bash"]
