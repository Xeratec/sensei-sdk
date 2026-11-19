# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# shellcheck shell=bash
# Sourced by build.sh and run.sh. Sets the paths; with --docker (default on macOS) the script runs again in the image
# sensei-bsim:ncs-3.4.1 (linux/amd64): the simulated boards are 32-bit x86 programs.
#
#   SENSEI_SDK_ROOT        SENSEI SDK (default: the repository of this script)
#   SENSEI_NCS_WORKSPACE   west workspace of NCS v3.4.1 (read-only; required)
#   SENSEI_BSIM_SRC        folder with tools/bsim and modules/bsim_hw_models/nrf_hw_models (default: bsim/babblesim-src,
#                          fetched by build.sh at the revisions of the workspace's zephyr/west.yml)
#   SENSEI_BSIM_BUILD      build and log folder (default: bsim/out)

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "${SENSEI_BSIM_IN_DOCKER:-}" ]; then
  SDK="${SENSEI_SDK_ROOT:-$(cd "$here/../../.." && pwd)}"
  : "${SENSEI_NCS_WORKSPACE:?set SENSEI_NCS_WORKSPACE to the west workspace of NCS v3.4.1}"
  WS="$SENSEI_NCS_WORKSPACE"
  BSIMSRC="${SENSEI_BSIM_SRC:-$here/babblesim-src}"
  OUT="${SENSEI_BSIM_BUILD:-$here/out}"
  mkdir -p "$OUT"
  OUT="$(cd "$OUT" && pwd)"
fi
image="${SENSEI_BSIM_IMAGE:-sensei-bsim:ncs-3.4.1}"

# Projects fetched by bsim_fetch: path url revision, as pinned by zephyr/west.yml of NCS v3.4.1 (group babblesim and
# nrf_hw_models). Order matters: the manifest (tools/bsim) contains the components.
BSIM_PROJECTS=(
  "tools/bsim/components https://github.com/BabbleSim/base 1cce925c72805cfdbb6341ca8c0755c7d42699d3"
  "tools/bsim/components/ext_2G4_channel_NtNcable https://github.com/BabbleSim/ext_2G4_channel_NtNcable 20a38c997f507b0aa53817aab3d73a462fff7af1"
  "tools/bsim/components/ext_2G4_channel_multiatt https://github.com/BabbleSim/ext_2G4_channel_multiatt bde72a57384dde7a4310bcf3843469401be93074"
  "tools/bsim/components/ext_2G4_device_WLAN_actmod https://github.com/BabbleSim/ext_2G4_device_WLAN_actmod 9cb6d8e72695f6b785e57443f0629a18069d6ce4"
  "tools/bsim/components/ext_2G4_device_burst_interferer https://github.com/BabbleSim/ext_2G4_device_burst_interferer 5b5339351d6e6a2368c686c734dc8b2fc65698fc"
  "tools/bsim/components/ext_2G4_device_playback https://github.com/BabbleSim/ext_2G4_device_playback abb48cd71ddd4e2a9022f4bf49b2712524c483e8"
  "tools/bsim/components/ext_2G4_libPhyComv1 https://github.com/BabbleSim/ext_2G4_libPhyComv1 23e7a84f5bc452a361ab30acf3abb9dbdb57e9a8"
  "tools/bsim/components/ext_2G4_modem_BLE_simple https://github.com/BabbleSim/ext_2G4_modem_BLE_simple 3712283f0bd3a982f620908579c99af35e969554"
  "tools/bsim/components/ext_2G4_modem_magic https://github.com/BabbleSim/ext_2G4_modem_magic d8281acb634895c8a769489c27146bdfe1a54938"
  "tools/bsim/components/ext_2G4_phy_v1 https://github.com/BabbleSim/ext_2G4_phy_v1 b0e49ae3dd4d6d0f2286fd1392cee1b5206f5bbc"
  "tools/bsim/components/ext_libCryptov1 https://github.com/BabbleSim/ext_libCryptov1 da246018ebe031e4fe4a8228187fb459e9f3b2fa"
  "tools/bsim https://github.com/zephyrproject-rtos/babblesim-manifest 7e10d556b5dd04360756cdf1f4b80d4f5bcac6cf"
  "modules/bsim_hw_models/nrf_hw_models https://github.com/zephyrproject-rtos/nrf_hw_models 158b9710d6e10c57b2c623f8f4178f0bbc85c23f"
)

# Fetch BabbleSim and the hardware models at those revisions (host side, needs git and network).
bsim_fetch() {
  local p path url rev dest
  for p in "${BSIM_PROJECTS[@]}"; do
    read -r path url rev <<<"$p"
    dest="$BSIMSRC/$path"
    git -C "$dest" rev-parse --verify -q "$rev^{commit}" >/dev/null 2>&1 && [ -d "$dest/.git" ] && continue
    mkdir -p "$dest"
    [ -d "$dest/.git" ] || git -C "$dest" init -q
    git -C "$dest" remote remove origin 2>/dev/null || true
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch -q --depth 1 origin "$rev"
    git -C "$dest" checkout -q FETCH_HEAD
    echo "fetched $path $rev"
  done
  # The hardware models do not generate the RTC TICK event that the library needs (patches/)
  dest="$BSIMSRC/modules/bsim_hw_models/nrf_hw_models"
  if ! git -C "$dest" apply --check -R "$here/patches/nrf_hw_models-rtc-tick.patch" 2>/dev/null; then
    git -C "$dest" apply "$here/patches/nrf_hw_models-rtc-tick.patch"
    echo "applied patches/nrf_hw_models-rtc-tick.patch"
  fi
}

# Run "$@" in the container. Workspace and SDK read-only; BabbleSim and the build folder writable.
bsim_in_docker() {
  local mounts=(-v "$WS:/workspace:ro" -v "$SDK:/sdk:ro" -v "$BSIMSRC/tools/bsim:/bsim" \
    -v "$BSIMSRC/modules/bsim_hw_models/nrf_hw_models:/hwmodels:ro" -v "$OUT:/out" -v "$here:/bsimtest:ro")
  docker image inspect "$image" >/dev/null 2>&1 || {
    echo "Image $image missing: build it from the Dockerfile of the sensei-ieq-fullstack bsim test (linux/amd64)" >&2; exit 1; }
  exec docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -e HOME=/out -e LOGNAME=bsim -e USER=bsim -e PYTHONDONTWRITEBYTECODE=1 \
    -e SENSEI_BSIM_IN_DOCKER=1 -e SENSEI_SDK_ROOT=/sdk \
    "${mounts[@]}" -w /workspace "$image" "$@"
}

if [ -n "${SENSEI_BSIM_IN_DOCKER:-}" ]; then
  WS=/workspace SDK=/sdk BSIM=/bsim HW=/hwmodels OUT=/out
else
  BSIM="$BSIMSRC/tools/bsim"
  HW="$BSIMSRC/modules/bsim_hw_models/nrf_hw_models"
fi
