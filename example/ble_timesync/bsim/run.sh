#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Run the BabbleSim time sync test: bs_2G4_phy_v1 with three devices, a master (crystal drift 0) and two followers
# (+40 ppm and -30 ppm), and analyze the PPS pin logs (analyze.py). Build first with build.sh.
#
#   export SENSEI_NCS_WORKSPACE=<west workspace of NCS v3.4.1>
#   example/ble_timesync/bsim/run.sh [--docker|--native] [--length S] [--skip S] [--threshold-us US]
#                                    [--drift-a PPM] [--drift-b PPM] [--rising-only]
#
# Logs: $SENSEI_BSIM_BUILD/logs/{phy,master,follower_a,follower_b}.log and gpio_*.csv (PPS pin logs), pairs.csv.
# Exit code 0 = all followers within the threshold.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mode=$([ "$(uname)" = Darwin ] && echo docker || echo native)
length=120
skip=20
threshold=5
drift_a=40
drift_b=-30
extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --docker) mode=docker; shift ;;
    --native) mode=native; shift ;;
    --length) length="$2"; shift 2 ;;
    --skip) skip="$2"; shift 2 ;;
    --threshold-us) threshold="$2"; shift 2 ;;
    --drift-a) drift_a="$2"; shift 2 ;;
    --drift-b) drift_b="$2"; shift 2 ;;
    --rising-only) extra+=(--rising-only); shift ;;
    -h | --help) sed -n '8,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

if [ -z "${SENSEI_BSIM_IN_DOCKER:-}" ] && [ "$mode" = docker ]; then
  bsim_in_docker bash /bsimtest/run.sh --native --length "$length" --skip "$skip" --threshold-us "$threshold" \
    --drift-a "$drift_a" --drift-b "$drift_b" ${extra[@]+"${extra[@]}"}
fi

sim_id="${SENSEI_BSIM_ID:-timesync}"
logs="$OUT/logs"
mkdir -p "$logs"
for f in "$OUT/master/zephyr/zephyr.exe" "$OUT/follower/zephyr/zephyr.exe" "$BSIM/bin/bs_2G4_phy_v1"; do
  [ -x "$f" ] || { echo "$f not found: run build.sh" >&2; exit 1; }
done
# A run that was killed leaves its pipes behind; the phy would not start on them.
rm -rf "/tmp/bs_${LOGNAME:-${USER:-user}}/${sim_id}"
rm -f "$logs"/*.log "$logs"/gpio_*.csv

# The XO model: -xo_drift=<fraction>, dev_time = (1 + drift) * phy_time. The GPIO model logs the device time (us).
(cd "$BSIM/bin" && exec ./bs_2G4_phy_v1 -s="$sim_id" -D=3 -sim_length="$((length * 1000000))") >"$logs/phy.log" 2>&1 &
pids=($!)
sleep 1
"$OUT/master/zephyr/zephyr.exe" -s="$sim_id" -d=0 -rs=1 -xo_drift=0 -gpio_out_file="$logs/gpio_master.csv" \
  >"$logs/master.log" 2>&1 &
pids+=($!)
"$OUT/follower/zephyr/zephyr.exe" -s="$sim_id" -d=1 -rs=2 -xo_drift="${drift_a}e-6" -gpio_out_file="$logs/gpio_follower_a.csv" \
  >"$logs/follower_a.log" 2>&1 &
pids+=($!)
"$OUT/follower/zephyr/zephyr.exe" -s="$sim_id" -d=2 -rs=3 -xo_drift="${drift_b}e-6" -gpio_out_file="$logs/gpio_follower_b.csv" \
  >"$logs/follower_b.log" 2>&1 &
pids+=($!)
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
[ "$rc" = 0 ] || echo "a simulation process failed (see $logs)" >&2

python3 -I "$(dirname "${BASH_SOURCE[0]}")/analyze.py" --master "$logs/gpio_master.csv" \
  --follower "A:$logs/gpio_follower_a.csv:${drift_a}" --follower "B:$logs/gpio_follower_b.csv:${drift_b}" \
  --skip "$skip" --threshold-us "$threshold" --csv "$logs/pairs.csv" ${extra[@]+"${extra[@]}"} || rc=1
exit "$rc"
