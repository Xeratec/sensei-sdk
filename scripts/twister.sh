#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Runs the Zephyr tests of the SDK (NRF/tests) with Twister on native_sim and, in Docker, on QEMU (qemu_cortex_m3 and
# mps2/an521/cpu0, for the fault capture tests). No hardware needed.
#
#   scripts/twister.sh [--host] [--coverage] [-- <twister arguments>]
#
# Default: in Docker (image sensei-twister:ncs-3.4.1, built on first use by scripts/docker/build.sh from
# scripts/docker/twister.Dockerfile on top of the toolchain image of scripts/docker/nrf.Dockerfile; it has the Zephyr
# SDK for the ARM build and QEMU). native_sim needs Linux and a host compiler, so this works on macOS too.
# --host: run Twister directly (Linux with gcc and a west workspace; this is what CI does). Only native_sim runs
# there unless TWISTER_PLATFORM adds a platform whose toolchain and emulator you have.
# --coverage: line and branch coverage of the SDK sources on native_sim (gcc gcov, gcovr). This run is native_sim only
# (the QEMU tests need no coverage, and their ARM gcov is not in the image); TWISTER_PLATFORM overrides. Result in <out>/twister-out/coverage/: index.html and coverage-summary.txt (one line per file).
# Vendor code in NRF/system/third_party is left out, except the MAX77654 driver. No threshold.
#
# Environment:
#   SENSEI_NCS_WORKSPACE  west workspace of NCS v3.4.1 with a venv or west in PATH
#                         (default: ~/Documents/work/Tools/sensei-ncs-v3.4.1). Mounted read-only.
#   TWISTER_OUT           output folder (default: <sdk>/build/twister). Deleted at the start.
#   TWISTER_PLATFORM      platforms, separated by spaces (default: native_sim/native/64 qemu_cortex_m3 mps2/an521/cpu0; with --host
#                         or --coverage native_sim/native/64 only)
#   TWISTER_IMAGE         Docker image (default sensei-twister:ncs-3.4.1)
#   TWISTER_DOCKER_ARGS   more arguments for docker run, for example "--memory=6g --cpus=4" (Twister's own jobs: -- -j4)
#   SENSEI_NRF_IMAGE      its base image, the toolchain (default sensei-nrf:ncs-3.4.1; scripts/docker/build.sh)
#
# Examples:
#   scripts/twister.sh
#   scripts/twister.sh --coverage
#   scripts/twister.sh -- -s sensei.secure.zephyr       # one test
#   scripts/twister.sh -- --list-tests

set -euo pipefail

sdk="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ws="${SENSEI_NCS_WORKSPACE:-$HOME/Documents/work/Tools/sensei-ncs-v3.4.1}"
out="${TWISTER_OUT:-$sdk/build/twister}"
image="${TWISTER_IMAGE:-sensei-twister:ncs-3.4.1}"
host=0

coverage=0
while [ "${1:-}" = "--host" ] || [ "${1:-}" = "--coverage" ]; do
  [ "$1" = "--host" ] && host=1
  [ "$1" = "--coverage" ] && coverage=1
  shift
done
if [ "$host" = 1 ] || [ "$coverage" = 1 ]; then
  platform="${TWISTER_PLATFORM:-native_sim/native/64}"
else
  platform="${TWISTER_PLATFORM:-native_sim/native/64 qemu_cortex_m3 mps2/an521/cpu0}"
fi
[ "${1:-}" = "--" ] && shift

mkdir -p "$out"
out="$(cd "$out" && pwd)"
rm -rf "${out:?}"/* "${out:?}"/.cache
mkdir "$out/.cache" # Zephyr caches compiler checks in $HOME/.cache (HOME=$out in the container)

# Twister arguments. -x passes the SDK as Zephyr module to every test build; --board-root lets a test name the SENSEI
# board (platform_allow), as secure_ocrypto does. A build that fails to configure because
# parallel jobs race on the compiler-check cache is run once more (--retry-build-errors); a test that fails is run once
# more too, so a real failure shows twice and a flake does not stop CI.
tw_args() {
  local sdk_dir="$1" out_dir="$2"
  local p
  TW=()
  for p in $platform; do TW+=(-p "$p"); done
  TW+=(-T "$sdk_dir/NRF/tests" -O "$out_dir" --inline-logs --no-detailed-test-id
    --retry-failed 1 --retry-build-errors
    "-x=EXTRA_ZEPHYR_MODULES=$sdk_dir/NRF" --board-root "$sdk_dir/NRF/boards")
  if [ "$coverage" = 1 ]; then
    # Coverage of native_sim only. The base directory is the SDK, so Zephyr and NCS files are not in the report.
    TW+=(--coverage --coverage-tool gcovr --coverage-platform native_sim/native/64 --coverage-basedir "$sdk_dir"
      --coverage-formats txt)
  fi
}

# Run after Twister with coverage: the report of the SDK sources from the gcovr data of all native_sim tests.
# $1 SDK dir, $2 Twister output dir
cov_report() {
  local base="$1" tw_out="$2"
  mkdir -p "$tw_out/coverage"
  gcovr -r "$base" --add-tracefile "$tw_out/coverage.json" --exclude '.*/third_party/(?!max77654/).*' \
    --txt "$tw_out/coverage/coverage-summary.txt" --html-details "$tw_out/coverage/index.html" --print-summary
}

if [ "$host" = 1 ]; then
  export ZEPHYR_BASE="$ws/zephyr"
  export ZEPHYR_TOOLCHAIN_VARIANT=host
  export PYTHONDONTWRITEBYTECODE=1
  tw_args "$sdk" "$out/twister-out"
  rc=0
  "$ws/zephyr/scripts/twister" "${TW[@]}" "$@" || rc=$?
  if [ "$coverage" = 1 ] && [ -f "$out/twister-out/coverage.json" ]; then
    cov_report "$sdk" "$out/twister-out" || rc=$?
  fi
  exit "$rc"
fi

command -v docker >/dev/null || { echo "docker not found (or use --host)" >&2; exit 1; }
TWISTER_IMAGE="$image" "$sdk/scripts/docker/build.sh" twister --if-missing

tw_args /sdk /out/twister-out
# The Zephyr SDK builds the ARM tests; native_sim uses the host compiler of the image. The shell around Twister runs
# the coverage report afterwards (gcovr is in the image).
in_container="$(declare -f cov_report); coverage=$coverage
rc=0
/workspace/zephyr/scripts/twister \"\$@\" || rc=\$?
if [ \"\$coverage\" = 1 ] && [ -f /out/twister-out/coverage.json ]; then cov_report /sdk /out/twister-out || rc=\$?; fi
exit \$rc"
# shellcheck disable=SC2086 # TWISTER_DOCKER_ARGS is a list of words
exec docker run --rm ${TWISTER_DOCKER_ARGS:-} --user "$(id -u):$(id -g)" -e HOME=/out \
  -e ZEPHYR_BASE=/workspace/zephyr -e ZEPHYR_TOOLCHAIN_VARIANT=zephyr -e PYTHONDONTWRITEBYTECODE=1 \
  -v "$ws:/workspace:ro" -v "$sdk:/sdk:ro" -v "$out:/out" -w /out \
  "$image" bash -c "$in_container" twister "${TW[@]}" "$@"
