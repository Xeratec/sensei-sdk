# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Checks that depend on the build type (Debug or Release). Included by sysbuild after the images are added.

# Debug and Release must not be mixed up: the app configuration and the signing key go together.
if("${EXTRA_CONF_FILE}" MATCHES "release\\.conf" AND NOT SB_CONFIG_SENSEI_RELEASE)
  message(FATAL_ERROR "A Release app configuration (conf/release.conf) needs -DSB_CONFIG_SENSEI_RELEASE=y, "
                      "so that MCUboot and the image are made with the Release key.")
endif()
if(SB_CONFIG_SENSEI_RELEASE AND NOT "${EXTRA_CONF_FILE}" MATCHES "release\\.conf")
  message(FATAL_ERROR "SB_CONFIG_SENSEI_RELEASE=y needs the Release app configuration: "
                      "-DEXTRA_CONF_FILE=conf/release.conf.")
endif()

# The kind of this board goes into the signed image as a protected TLV (covered by the signature). The shield checks it
# before it accepts an image (an image of the nRF5340 node must not run on the nRF9151), and the host tools check it
# too. The app takes the string from these arguments (CMakeLists.txt). The nRF5340 node has the same with its own kind.
set_config_string(${DEFAULT_IMAGE} CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS "--custom-tlv 0xa0 sensei-lpwan-nrf9151")

if(SB_CONFIG_SENSEI_RELEASE)
  # The Release key is private and stays outside git.
  if("${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}" STREQUAL "")
    message(FATAL_ERROR "Release build: no MCUboot signing key. Set the environment variable "
                        "SENSEI_SHIELD_MCUBOOT_KEY to a PEM file (ECDSA P-256, not the key of the nRF5340 node) or pass "
                        "-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=<file>. Make a key with: "
                        "imgtool keygen -k <file> -t ecdsa-p256. See README.md.")
  endif()
  if(NOT EXISTS "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
    message(FATAL_ERROR "Release build: the MCUboot signing key '${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}' "
                        "does not exist. See README.md.")
  endif()
  # The shield and the nRF5340 node have different keys: a signed image of one is then not valid for the other.
  if(DEFINED ENV{SENSEI_MCUBOOT_KEY} AND EXISTS "$ENV{SENSEI_MCUBOOT_KEY}")
    file(SHA256 "$ENV{SENSEI_MCUBOOT_KEY}" node_key_hash)
    file(SHA256 "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}" shield_key_hash)
    if(node_key_hash STREQUAL shield_key_hash)
      message(FATAL_ERROR "Release build: the shield key is the same file content as SENSEI_MCUBOOT_KEY (the nRF5340 "
                          "node). Make a key of its own for the shield: imgtool keygen -k <file> -t ecdsa-p256.")
    endif()
  endif()
endif()
