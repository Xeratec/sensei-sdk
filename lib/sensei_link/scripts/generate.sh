#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
#
# Regenerate the nanopb C files of the SDK schemas (proto/sensei/*.proto) into generated/sensei/.
#   scripts/generate.sh           write the generated files
#   scripts/generate.sh --check   regenerate into a temp dir and fail if the committed files differ
#
# The generator is the one of the nanopb submodule (third_party/nanopb), the same version as the
# runtime (see README.md). It needs Python with protobuf and grpcio-tools, for example a venv:
#   python3 -m venv <venv> && <venv>/bin/pip install protobuf grpcio-tools
#   NANOPB_GENERATOR="<venv>/bin/python third_party/nanopb/generator/nanopb_generator.py" scripts/generate.sh
# NANOPB_GENERATOR is a command line (a script, or "python <script>").
set -euo pipefail

lib="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gen="${NANOPB_GENERATOR:-python3 $lib/third_party/nanopb/generator/nanopb_generator.py}"
c_dir="$lib/generated"

mode=write
[[ "${1:-}" == "--check" ]] && mode=check

if [[ $mode == check ]]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    out="$tmp/generated"
else
    out="$c_dir"
fi
mkdir -p "$out"

cd "$lib/proto"
# shellcheck disable=SC2086
$gen -I "$lib/proto" -D "$out" sensei/*.proto

spdx_c='/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */
'
for f in "$out"/sensei/*.pb.[ch]; do
    { printf '%s\n' "$spdx_c"; cat "$f"; } > "$f.new" && mv "$f.new" "$f"
done

if [[ $mode == check ]]; then
    if ! diff -r "$out" "$c_dir"; then
        echo "generated files are out of date: run lib/sensei_link/scripts/generate.sh" >&2
        exit 1
    fi
    echo "generated files are current"
fi
