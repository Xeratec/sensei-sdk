# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Image for scripts/twister.sh: the nRF build image of the node repository (Zephyr SDK, west, Python
# requirements of NCS v3.4.1) plus the host compiler that native_sim needs and QEMU for the Cortex-M tests
# (qemu_cortex_m3, lm3s6965evb). Build it with:
#   docker build -f scripts/docker/twister.Dockerfile -t sensei-twister:ncs-3.4.1 scripts/docker
ARG BASE=sensei-nrf:ncs-3.4.1
FROM ${BASE}

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        g++ \
        libc6-dev \
        qemu-system-arm \
    && rm -rf /var/lib/apt/lists/*
