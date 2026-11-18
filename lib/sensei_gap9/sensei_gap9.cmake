# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# GAP9 side of the link to the nRF5340, for GAP9 apps (they build with the GAP SDK, so the sources go into
# the app's own target and get its include paths):
#
#   include($ENV{SENSEI_SDK_ROOT}/lib/sensei_gap9/sensei_gap9.cmake)
#   target_sources(app PRIVATE ${SENSEI_GAP9_SIDE_SOURCES})
#   target_include_directories(app PRIVATE ${SENSEI_GAP9_SIDE_INCLUDE_DIRS})
#
# It also needs the sensei_link sources (sensei_link.cmake). The nRF5340 side is the Zephyr library
# (CONFIG_SENSEI_GAP9).

set(SENSEI_GAP9_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(SENSEI_GAP9_SIDE_SOURCES ${SENSEI_GAP9_DIR}/gap9/gap9_side.c)
set(SENSEI_GAP9_SIDE_INCLUDE_DIRS ${SENSEI_GAP9_DIR}/include)

# Update of the GAP9 application (OTA.md): the writer and the MRAM backend of GAP9. The backend wraps
# pi_fs_conf_init so that the app opens the readfs of its own slot: link with SENSEI_GAP9_OTA_LINK_OPTIONS. The
# writer needs the image helpers of NRF/system/ota (header only).
#
#   target_sources(app PRIVATE ${SENSEI_GAP9_OTA_SIDE_SOURCES})
#   target_include_directories(app PRIVATE ${SENSEI_GAP9_OTA_INCLUDE_DIRS})
#   target_link_options(app PRIVATE ${SENSEI_GAP9_OTA_LINK_OPTIONS})
set(SENSEI_GAP9_OTA_SIDE_SOURCES ${SENSEI_GAP9_DIR}/src/g9ota.c ${SENSEI_GAP9_DIR}/src/g9ota_ckpt.c
    ${SENSEI_GAP9_DIR}/gap9/g9ota_mram_ops.c
    ${SENSEI_GAP9_DIR}/gap9/g9ota_mram.c)
set(SENSEI_GAP9_OTA_INCLUDE_DIRS ${SENSEI_GAP9_DIR}/include ${SENSEI_GAP9_DIR}/../../NRF/system/ota)
set(SENSEI_GAP9_OTA_LINK_OPTIONS -Wl,--wrap=pi_fs_conf_init)

# Recovery in the boot loader (GAP9/ssbl, OTA.md "Recovery"): the writer without the hand-off arming and the readfs
# wrapper of an app, and the recovery job server. The SSBL also needs the link side (SENSEI_GAP9_SIDE_SOURCES) and
# sensei_link.
set(SENSEI_GAP9_RECOVERY_SOURCES ${SENSEI_GAP9_DIR}/src/g9ota.c ${SENSEI_GAP9_DIR}/src/g9ota_ckpt.c
    ${SENSEI_GAP9_DIR}/src/g9rec.c ${SENSEI_GAP9_DIR}/gap9/g9ota_mram_ops.c)
