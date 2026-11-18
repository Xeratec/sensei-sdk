# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_SECURE is set.
set(SENSEI_SECURE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_secure)
zephyr_library_sources(
  ${SENSEI_SECURE_DIR}/src/sensei_secure.c
  ${SENSEI_SECURE_DIR}/src/sensei_e2e.c
  ${SENSEI_SECURE_DIR}/src/sensei_secure_zephyr.c
)
zephyr_include_directories(${SENSEI_SECURE_DIR}/include)
# <crypto/sensei_crypto.h> of the SDK crypto library
zephyr_library_include_directories(${SENSEI_SECURE_DIR}/../../NRF/system)
zephyr_compile_definitions(SENSEI_SECURE_BACKEND_ZEPHYR)
zephyr_library_sources_ifdef(CONFIG_SENSEI_SECURE_SETTINGS ${CMAKE_CURRENT_LIST_DIR}/sensei_e2e_settings.c)
