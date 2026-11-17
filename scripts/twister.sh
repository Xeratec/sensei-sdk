#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Runs the Zephyr tests of the SDK (NRF/tests) with Twister on native_sim. No hardware needed.
#
#   scripts/twister.sh [--host] [-- <twister arguments>]
#
# Default: in Docker (image sensei-twister:ncs-3.4.1, built on first use from scripts/docker/twister.Dockerfile).
# native_sim needs Linux and a host compiler, so this works on macOS too.
# --host: run Twister directly (Linux with gcc and a west workspace; this is what CI does).
#
# Environment:
#   SENSEI_NCS_WORKSPACE  west workspace of NCS v3.4.1 with a venv or west in PATH
#                         (default: ~/Documents/work/Tools/sensei-ncs-v3.4.1). Mounted read-only.
#   TWISTER_OUT           output folder (default: <sdk>/build/twister). Deleted at the start.
#   TWISTER_PLATFORM      default native_sim/native/64
#   TWISTER_IMAGE         Docker image (default sensei-twister:ncs-3.4.1)
#
# Examples:
#   scripts/twister.sh
#   scripts/twister.sh -- -s sensei.secure.zephyr       # one test
#   scripts/twister.sh -- --list-tests

set -euo pipefail

sdk="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ws="${SENSEI_NCS_WORKSPACE:-$HOME/Documents/work/Tools/sensei-ncs-v3.4.1}"
out="${TWISTER_OUT:-$sdk/build/twister}"
platform="${TWISTER_PLATFORM:-native_sim/native/64}"
image="${TWISTER_IMAGE:-sensei-twister:ncs-3.4.1}"
host=0

if [ "${1:-}" = "--host" ]; then
  host=1
  shift
fi
[ "${1:-}" = "--" ] && shift

mkdir -p "$out"
out="$(cd "$out" && pwd)"
rm -rf "${out:?}"/* "${out:?}"/.cache
mkdir "$out/.cache" # Zephyr caches compiler checks in $HOME/.cache (HOME=$out in the container)

# Twister arguments. -x passes the SDK as Zephyr module to every test build.
tw_args() {
  local sdk_dir="$1" out_dir="$2"
  TW=(-p "$platform" -T "$sdk_dir/NRF/tests" -O "$out_dir" --inline-logs --no-detailed-test-id
    "-x=EXTRA_ZEPHYR_MODULES=$sdk_dir/NRF")
}

if [ "$host" = 1 ]; then
  export ZEPHYR_BASE="$ws/zephyr"
  export ZEPHYR_TOOLCHAIN_VARIANT=host
  export PYTHONDONTWRITEBYTECODE=1
  tw_args "$sdk" "$out/twister-out"
  exec "$ws/zephyr/scripts/twister" "${TW[@]}" "$@"
fi

command -v docker >/dev/null || { echo "docker not found (or use --host)" >&2; exit 1; }
if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "Image $image missing: building it from scripts/docker/twister.Dockerfile" >&2
  echo "(its base sensei-nrf:ncs-3.4.1 comes from firmware/scripts/docker/nrf.Dockerfile of the node repository)" >&2
  docker build -f "$sdk/scripts/docker/twister.Dockerfile" -t "$image" "$sdk/scripts/docker"
fi

tw_args /sdk /out/twister-out
# Zephyr uses the host compiler (no cross toolchain) for native_sim.
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/out \
  -e ZEPHYR_BASE=/workspace/zephyr -e ZEPHYR_TOOLCHAIN_VARIANT=host -e PYTHONDONTWRITEBYTECODE=1 \
  -v "$ws:/workspace:ro" -v "$sdk:/sdk:ro" -v "$out:/out" -w /out \
  "$image" /workspace/zephyr/scripts/twister "${TW[@]}" "$@"
