# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Build image for nRF firmware, and the base of the other SDK images (nRF Connect SDK v3.4.1, Zephyr SDK 1.0.1).
# It holds the toolchain and the Python packages only. The sources come from a west workspace
# that you mount at /workspace (scripts/docker/README.md).
# Build it with scripts/docker/build.sh nrf, or with the context scripts (the constraints file is there):
#   docker build -f scripts/docker/nrf.Dockerfile -t sensei-nrf:ncs-3.4.1 scripts

# Ubuntu 24.04 by digest (index of all platforms), so a rebuild starts from the same base. To move to a newer
# base: docker buildx imagetools inspect ubuntu:24.04, copy the digest, rebuild and test.
FROM ubuntu:24.04@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8

ARG DEBIAN_FRONTEND=noninteractive
ARG ZEPHYR_SDK_VERSION=1.0.1
# Same versions as the setup script of an app that builds on a local west workspace (west, CMake)
ARG WEST_VERSION=1.5.0
ARG CMAKE_VERSION=4.4.5
# Python requirements are taken from the tags that west.yml pins (sdk-nrf).
ARG ZEPHYR_TAG=ncs-v3.4.1
ARG MCUBOOT_TAG=ncs-v3.4.1
ARG NCS_TAG=v3.4.1

# The packages are those of the Ubuntu archive of the build day (exact versions do not last: the archive
# replaces them). The build tools that matter are pinned below: Zephyr SDK, west, CMake (pip, first in PATH;
# the apt one only registers the Zephyr SDK), Python requirements.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        curl \
        device-tree-compiler \
        git \
        gperf \
        make \
        ninja-build \
        python3 \
        python3-pip \
        python3-venv \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Zephyr SDK: minimal bundle and the ARM toolchain, checked against the release checksums.
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64) platform=linux-x86_64 ;; \
      aarch64) platform=linux-aarch64 ;; \
      *) echo "unsupported architecture" >&2; exit 1 ;; \
    esac; \
    base="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}"; \
    minimal="zephyr-sdk-${ZEPHYR_SDK_VERSION}_${platform}_minimal.tar.xz"; \
    toolchain="toolchain_gnu_${platform}_arm-zephyr-eabi.tar.xz"; \
    cd /tmp; \
    curl -fLO "${base}/sha256.sum" -O "${base}/${minimal}" -O "${base}/${toolchain}"; \
    grep -E " (${minimal}|${toolchain})\$" sha256.sum | sha256sum -c -; \
    mkdir -p /opt/zephyr-sdk/gnu; \
    tar -xJf "${minimal}" -C /opt/zephyr-sdk --strip-components=1; \
    tar -xJf "${toolchain}" -C /opt/zephyr-sdk/gnu; \
    cmake -P /opt/zephyr-sdk/cmake/zephyr_sdk_export.cmake; \
    rm -f /tmp/sha256.sum "/tmp/${minimal}" "/tmp/${toolchain}"

# West, CMake and the Python requirements of Zephyr, nRF Connect SDK and MCUboot (versions from the constraints file).
COPY requirements-constraints.txt /tmp/requirements-constraints.txt
RUN set -eux; \
    python3 -m venv /opt/venv; \
    /opt/venv/bin/pip install --no-cache-dir "west==${WEST_VERSION}" "cmake==${CMAKE_VERSION}"; \
    mkdir -p /tmp/req/zephyr /tmp/req/nrf /tmp/req/mcuboot; \
    for f in requirements.txt requirements-base.txt requirements-build-test.txt \
             requirements-run-test.txt requirements-extras.txt requirements-compliance.txt; do \
      curl -fL -o "/tmp/req/zephyr/${f}" \
        "https://raw.githubusercontent.com/nrfconnect/sdk-zephyr/${ZEPHYR_TAG}/scripts/${f}"; \
    done; \
    for f in requirements.txt requirements-base.txt requirements-build.txt requirements-doc.txt \
             requirements-ci.txt; do \
      curl -fL -o "/tmp/req/nrf/${f}" \
        "https://raw.githubusercontent.com/nrfconnect/sdk-nrf/${NCS_TAG}/scripts/${f}" || true; \
    done; \
    curl -fL -o /tmp/req/mcuboot/requirements.txt \
      "https://raw.githubusercontent.com/nrfconnect/sdk-mcuboot/${MCUBOOT_TAG}/scripts/requirements.txt"; \
    /opt/venv/bin/pip install --no-cache-dir -c /tmp/requirements-constraints.txt \
      -r /tmp/req/zephyr/requirements.txt \
      -r /tmp/req/nrf/requirements.txt \
      -r /tmp/req/mcuboot/requirements.txt; \
    rm -rf /tmp/req /tmp/requirements-constraints.txt

# The workspace is mounted from the host and owned by another user.
RUN git config --system --add safe.directory '*'

ENV VENV=/opt/venv \
    PATH=/opt/venv/bin:${PATH} \
    ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
    ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk

WORKDIR /workspace
