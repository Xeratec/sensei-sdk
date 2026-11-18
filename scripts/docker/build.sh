#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Builds the Docker images of the SDK. Apps build theirs on these (the node repository's simulator image is made from
# the twister image).
#
#   scripts/docker/build.sh nrf       [--if-missing]   toolchain image (nrf.Dockerfile)
#   scripts/docker/build.sh twister   [--if-missing]   nrf image plus host compiler and QEMU (twister.Dockerfile)
#
# --if-missing: build only if the image does not exist (twister builds the nrf image first if that is missing).
#
# Environment:
#   SENSEI_NRF_IMAGE  name of the nrf image (default sensei-nrf:ncs-3.4.1)
#   TWISTER_IMAGE     name of the twister image (default sensei-twister:ncs-3.4.1)

set -euo pipefail

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
nrf_image="${SENSEI_NRF_IMAGE:-sensei-nrf:ncs-3.4.1}"
twister_image="${TWISTER_IMAGE:-sensei-twister:ncs-3.4.1}"

target="${1:-}"
missing_only=0
[ "${2:-}" = "--if-missing" ] && missing_only=1
command -v docker >/dev/null || { echo "docker not found" >&2; exit 1; }

have() { docker image inspect "$1" >/dev/null 2>&1; }

build_nrf() {
  if [ "$missing_only" = 1 ] && have "$nrf_image"; then return; fi
  echo "Building $nrf_image from scripts/docker/nrf.Dockerfile" >&2
  # the context is scripts/: the constraints file is there
  docker build -f "$dir/nrf.Dockerfile" -t "$nrf_image" "$dir/.."
}

build_twister() {
  if [ "$missing_only" = 1 ] && have "$twister_image"; then return; fi
  missing_only=1 build_nrf
  echo "Building $twister_image from scripts/docker/twister.Dockerfile (base $nrf_image)" >&2
  docker build -f "$dir/twister.Dockerfile" --build-arg "BASE=$nrf_image" -t "$twister_image" "$dir"
}

case "$target" in
  nrf) build_nrf ;;
  twister) build_twister ;;
  *) sed -n '8,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
