#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Build the BabbleSim time sync test: BabbleSim (fetched at the revisions of the NCS workspace), the network core
# (NCS ipc_radio with the options of sysbuild/ipc_radio.conf) and the example as master and as follower on the
# simulated nRF5340. Run it with run.sh.
#
#   export SENSEI_NCS_WORKSPACE=<west workspace of NCS v3.4.1>
#   example/ble_timesync/bsim/build.sh [--docker|--native] [--pulse-us US]
#
# --pulse-us: the PPS output is a pulse of that width (sensei_ts_pps_start_pulse) instead of a 50 % toggle; use a
# separate SENSEI_BSIM_BUILD for it and run.sh --rising-only.
#
# Default: --docker on macOS, --native elsewhere (Linux x86_64 with gcc-multilib and g++-multilib). Result in
# $SENSEI_BSIM_BUILD (default bsim/out): net, master, follower. See common.sh for the variables.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mode=$([ "$(uname)" = Darwin ] && echo docker || echo native)
pulse_us=0
while [ $# -gt 0 ]; do
  case "$1" in
    --docker) mode=docker; shift ;;
    --native) mode=native; shift ;;
    --pulse-us) pulse_us="$2"; shift 2 ;;
    -h | --help) sed -n '8,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

if [ -z "${SENSEI_BSIM_IN_DOCKER:-}" ]; then
  bsim_fetch
  [ "$mode" = docker ] && bsim_in_docker bash /bsimtest/build.sh --native --pulse-us "$pulse_us"
fi

# The simulated boards use the host compiler (32-bit), not the Zephyr SDK.
export ZEPHYR_TOOLCHAIN_VARIANT=host
unset ZEPHYR_BASE
export BSIM_OUT_PATH="$BSIM" BSIM_COMPONENTS_PATH="$BSIM/components"
[ -z "${SENSEI_BSIM_IN_DOCKER:-}" ] && [ -d "$WS/.venv/bin" ] && export PATH="$WS/.venv/bin:$PATH"
echo 'int main(void) { return 0; }' | gcc -m32 -x c - -o /dev/null 2>/dev/null ||
  { echo "gcc -m32 does not work: install gcc-multilib and g++-multilib" >&2; exit 1; }
export SENSEI_SDK_ROOT="$SDK"
ex="$SDK/example/ble_timesync/src_NRF"
modules="$SDK/NRF;$HW"

# 1. BabbleSim: the radio simulator and its libraries
if [ ! -x "$BSIM/bin/bs_2G4_phy_v1" ] || [ ! -e "$BSIM/lib/libUtilv1.32.a" ]; then
  make -C "$BSIM" everything -j"$(nproc 2>/dev/null || echo 4)"
fi

cd "$WS"
# 2. Network core: HCI over IPC with the SoftDevice Controller (NCS ships its BabbleSim build), as on the board
west build -b nrf5340bsim/nrf5340/cpunet "$WS/nrf/applications/ipc_radio" -d "$OUT/net" --no-sysbuild --pristine auto -- \
  "-DEXTRA_ZEPHYR_MODULES=$HW;$here/netshim" \
  "-DEXTRA_CONF_FILE=$WS/nrf/applications/ipc_radio/overlay-bt_hci_ipc.conf;$ex/sysbuild/ipc_radio.conf"

# 3. The example on the application core, one build per role; the network core image is linked into the executable
for role in master follower; do
  west build -b nrf5340bsim/nrf5340/cpuapp "$ex" -d "$OUT/$role" --no-sysbuild --pristine auto -- \
    "-DEXTRA_ZEPHYR_MODULES=$modules" "-DEXTRA_CONF_FILE=$ex/$role.conf" "-DCONFIG_APP_PPS_PULSE_WIDTH_US=$pulse_us" \
    "-DCONFIG_NATIVE_SIMULATOR_EXTRA_IMAGE_PATHS=\"$OUT/net/zephyr/zephyr.elf\""
done
echo "built: $OUT/{master,follower}/zephyr/zephyr.exe"
