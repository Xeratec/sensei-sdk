#!/bin/bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# SSBL test on GVSoC, inside the GAP9 Docker container (run it with tools/ssbl_docker.sh test). It builds a test app
# twice (slot A and B), packs them into slot images, builds the SSBL for GVSoC with a forced slot and runs it on
# the MRAM image (FSBL, SSBL, both slots). Cases: each slot boots, a damaged slot falls back to the other, no slot.
# The test app prints the slot of the hand-off record: after a fallback it names the slot that really started.
# Recovery cases (SSBL built with SENSEI_SSBL_RECOVERY and the test feed test/recovery_feed.c: GVSoC has no nRF5340 on
# the SPI link, the jobs come from a packed image in the keep slot): rec_both_bad (neither slot boots: the writer writes
# the other slot, seal, the SSBL boots it), rec_no_seal (no seal: the slot stays unbootable), rec_forced (the strap is high:
# a good slot A boots after the recovery wrote slot B).
set -uo pipefail

SDK=/sensei-sdk
W=/tmp/w
JOBS=${BUILD_JOBS:-4}
PACK=$SDK/GAP9/ssbl/tools/pack_slot.py
mkdir -p $W
cp -r $SDK/GAP9/ssbl/test/app $W/app_src

fail=0
for id in A B; do
    cmake -B $W/app_$id $W/app_src -DAPP_ID=$id -DSENSEI_SDK_DIR=$SDK >$W/app_$id.cmake.log 2>&1 &&
        cmake --build $W/app_$id -j "$JOBS" >$W/app_$id.build.log 2>&1 &&
        cmake --build $W/app_$id -t image >$W/app_$id.image.log 2>&1 &&
        cp $W/app_$id/chip.soc.mram.bin $W/app_$id.bin || { echo "app $id: build failed"; tail -20 $W/app_$id.*.log; exit 1; }
done
ls -l $W/app_A.bin $W/app_B.bin

# The packed image for the other slot, as the test feed reads it from the keep slot: u32 "FEED", u32 length, 8 B, the file.
# It has a TLV of the shape of a signature (the writer checks the shape, the nRF5340 the signature: --test-signature).
feed_into_a() {
    python3 -I "$PACK" --app $W/app_B.bin --kind sensei-ssbl-test --version 2.0.0+2 --test-signature -o $W/feed_img.bin >/dev/null &&
        python3 -I - $W/slot_a.bin $W/feed_img.bin <<'PY'
import struct, sys
slot = bytearray(open(sys.argv[1], 'rb').read())
img = open(sys.argv[2], 'rb').read()
FEED_OFF = 0x80000
assert len(slot) < FEED_OFF and FEED_OFF + 16 + len(img) < 0xF6000
slot += bytes([0xFF]) * (FEED_OFF - len(slot))
slot += struct.pack('<II', 0x44454546, len(img)) + bytes(8) + img
open(sys.argv[1], 'wb').write(slot)
PY
}

mk() { python3 -I "$PACK" --app $W/app_$1.bin --kind sensei-ssbl-test --version "$2" --damage "$3" -o $W/slot_$4.bin >/dev/null; }

# case name | forced slot | slot A damage | slot B damage | must contain (in order) | must not contain
run_case() {
    local name=$1 force=$2 da=$3 db=$4 want=$5 not=$6 extra=${7:-}
    mk A 1.0.0+1 "$da" a
    mk B 2.0.0+2 "$db" b
    if [ -n "$extra" ]; then feed_into_a; fi
    rm -rf $W/ssbl
    # shellcheck disable=SC2086
    cmake -S $SDK/GAP9/ssbl -B $W/ssbl -DSENSEI_PLATFORM=gvsoc -DSENSEI_SSBL_FORCE_SLOT=$force \
        -DSENSEI_SSBL_SLOT_A=$W/slot_a.bin -DSENSEI_SSBL_SLOT_B=$W/slot_b.bin $extra >$W/ssbl.cmake.log 2>&1 &&
        cmake --build $W/ssbl -j "$JOBS" >$W/ssbl.build.log 2>&1 || { echo "$name: SSBL build failed"; tail -20 $W/ssbl.*.log; fail=1; return; }
    timeout 120 cmake --build $W/ssbl -t run >$W/$name.log 2>&1
    local ok=1 pos=0
    IFS='|' read -ra lines <<<"$want"
    for l in "${lines[@]}"; do
        if ! grep -q -- "$l" $W/$name.log; then ok=0; echo "  missing: $l"; fi
    done
    if [ -n "$not" ] && grep -q -- "$not" $W/$name.log; then ok=0; echo "  unexpected: $not"; fi
    if [ $ok = 1 ]; then echo "PASS $name"; else echo "FAIL $name"; grep -v "^\[.*Built target" $W/$name.log | tail -25; fail=1; fi
}

# The layout: the slots are where gap9_slot.h says (the sizes of the boot area sections are fixed in the layout).
cmake -S $SDK/GAP9/ssbl -B $W/lay -DSENSEI_PLATFORM=board >$W/lay.cmake.log 2>&1 &&
    cmake --build $W/lay -j "$JOBS" >$W/lay.build.log 2>&1 &&
    cmake --build $W/lay -t flash_layout >$W/lay.log 2>&1
for want in "0xe70 .*ssbl .*0xc000" "0xd270 .*volume_table .*0x2d10" "0x10000 .*checkpoint" "0x14000 .*slot_a" "0x10a000 .*slot_b"; do
    if grep -q -E "^\| $want" $W/lay.log; then echo "PASS layout: $want"; else echo "FAIL layout: $want"; fail=1; fi
done

# The boot-only layout (make flash-boot): the same boot area, no checkpoint and no slots.
cmake -S $SDK/GAP9/ssbl -B $W/layb -DSENSEI_PLATFORM=board -DSENSEI_SSBL_BOOT_ONLY=ON >$W/layb.cmake.log 2>&1 &&
    cmake --build $W/layb -j "$JOBS" >$W/layb.build.log 2>&1 &&
    cmake --build $W/layb -t flash_layout >$W/layb.log 2>&1
if grep -q -E "^\| 0xff80 .*readfs_mram" $W/layb.log && ! grep -q -E "slot_a|slot_b|checkpoint" $W/layb.log; then
    echo "PASS layout: boot area only"
else
    echo "FAIL layout: boot area only"
    fail=1
fi

run_case boot_a 0 none none "boot slot A|slot A running (42), hand-off A" "slot B running"
run_case boot_b 1 none none "boot slot B|slot B running (42), hand-off B" "slot A running"
run_case b_bad_crc 1 none app "try slot B|slot B: app load failed|boot slot A|slot A running (42), hand-off A" "slot B running"
run_case b_erased 1 none header "slot B: not bootable (-2)|boot slot A|slot A running (42), hand-off A" "slot B running"
run_case b_partial 1 none trailer "slot B: not bootable (-77)|boot slot A|slot A running (42), hand-off A" "slot B running"
run_case a_bad_crc 0 app none "slot A: app load failed|boot slot B|slot B running (42), hand-off B" "slot A running"
run_case both_bad 0 header trailer "slot A: not bootable|slot B: not bootable|no bootable slot" "running"

# Recovery. The kind of the test images is sensei-ssbl-test (the writer refuses another).
REC="-DSENSEI_SSBL_RECOVERY=ON -DSENSEI_SSBL_KIND=sensei-ssbl-test -DSENSEI_SSBL_RECOVERY_TEST=ON"
run_case rec_both_bad 0 header trailer "slot A: not bootable|slot B: not bootable|no bootable slot|recovery: both slots failed|recovery: keep slot A, reason 1|recovery feed: begin ok|recovery feed: finish ok, not sealed|recovery feed: sealed slot B|recovery feed: slot B parses|boot slot B|slot B running (42), hand-off B" "slot A running" "$REC"
run_case rec_no_seal 0 header trailer "recovery: both slots failed|recovery feed: begin ok|recovery feed: finish ok, not sealed|slot B: not bootable|no bootable slot" "sealed slot|boot slot B" "$REC -DSENSEI_SSBL_RECOVERY_TEST_NOSEAL=ON"
run_case rec_forced 0 none none "recovery forced|recovery: keep slot A, reason 2|recovery feed: begin ok|recovery feed: sealed slot B|recovery feed: slot B parses|boot slot A|slot A running (42), hand-off A" "slot B running|no bootable slot" "$REC -DSENSEI_SSBL_RECOVERY_TEST_STRAP=ON"

if [ $fail = 0 ]; then echo "SSBL TEST PASS"; else echo "SSBL TEST FAIL"; exit 1; fi
