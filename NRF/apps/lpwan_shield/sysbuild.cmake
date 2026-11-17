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

if(SB_CONFIG_SENSEI_RELEASE)
  # The Release key is private and stays outside git.
  if("${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}" STREQUAL "")
    message(FATAL_ERROR "Release build: no MCUboot signing key. Set the environment variable "
                        "SENSEI_SHIELD_MCUBOOT_KEY (or SENSEI_MCUBOOT_KEY) to a PEM file (ECDSA P-256) or pass "
                        "-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=<file>. Make a key with: "
                        "imgtool keygen -k <file> -t ecdsa-p256. See README.md.")
  endif()
  if(NOT EXISTS "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
    message(FATAL_ERROR "Release build: the MCUboot signing key '${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}' "
                        "does not exist. See README.md.")
  endif()
endif()
