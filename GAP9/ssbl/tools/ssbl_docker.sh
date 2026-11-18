#!/bin/bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Build the SSBL or run its GVSoC test in a throw-away Docker container (the SDK is mounted read-only).
#
#   tools/ssbl_docker.sh build <outdir>   board build: <outdir>/ssbl (ELF)
#   tools/ssbl_docker.sh test             GVSoC test (test/gvsoc_cases.sh): prints SSBL TEST PASS or FAIL
#
# Environment: IMAGE is the Docker image (default gap9:v5.22.4-sensei), BUILD_JOBS (default 4).
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SDK=$(cd "$HERE/../../.." && pwd)
IMAGE=${IMAGE:-gap9:v5.22.4-sensei}
MODE=${1:-test}
OUT=""
MOUNT=()
if [ "$MODE" = build ]; then
    OUT=${2:?usage: ssbl_docker.sh build <outdir>}
    mkdir -p "$OUT"
    OUT=$(cd "$OUT" && pwd)
    MOUNT=(-v "$OUT":/out)
    CMD='cmake -S /sensei-sdk/GAP9/ssbl -B /tmp/b >/tmp/cmake.log 2>&1 || { tail -30 /tmp/cmake.log; exit 1; }
         cmake --build /tmp/b -j ${BUILD_JOBS:-4} >/tmp/build.log 2>&1 || { grep -n -i error /tmp/build.log | head; exit 1; }
         cp /tmp/b/ssbl /out/ssbl
         riscv32-unknown-elf-size /out/ssbl'
elif [ "$MODE" = test ]; then
    CMD='/sensei-sdk/GAP9/ssbl/test/gvsoc_cases.sh'
else
    echo "usage: $0 build <outdir> | test" >&2
    exit 1
fi

docker run --rm -v "$SDK":/sensei-sdk:ro ${MOUNT[@]+"${MOUNT[@]}"} -e BUILD_JOBS="${BUILD_JOBS:-4}" "$IMAGE" bash -c "
    set +u
    export GAP_SDK_HOME=/app/install/gap9-sdk GAP_RISCV_GCC_TOOLCHAIN=/app/install/gap9-gcc
    source \$GAP_SDK_HOME/configs/gap9_evk_audio.sh >/dev/null 2>&1
    export PATH=\"\$GAP_SDK_HOME/utils/gapy_v2/bin:\$GAP_SDK_HOME/utils/bin:\$GAP_RISCV_GCC_TOOLCHAIN/bin:\$GAP_SDK_HOME/install/workstation/bin:\$GAP_SDK_HOME/.gap9-venv/bin:\$PATH\"
    export VIRTUAL_ENV=\$GAP_SDK_HOME/.gap9-venv
    set -e
    $CMD
"
