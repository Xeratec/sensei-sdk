# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# SENSEI LPWAN shield firmware for the nRF9151 (NCS v3.4.1). It serves the nRF5340 over SPI.
# Board: sensei_lpwan_nrf9151/nrf9151/ns, from the SENSEI SDK.
#   export SENSEI_SDK_ROOT=<path to the SENSEI SDK>
#   cd $SENSEI_SDK_ROOT/NRF/apps/lpwan_shield
#   west build -b sensei_lpwan_nrf9151/nrf9151/ns -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF \
#     -DEXTRA_CONF_FILE=conf/debug.conf

cmake_minimum_required(VERSION 3.20.0)

if(NOT DEFINED ENV{SENSEI_SDK_ROOT})
  message(FATAL_ERROR "Set SENSEI_SDK_ROOT to the SENSEI SDK.")
endif()

# The SDK provides the board, the devicetree bindings and the libraries sensei_link and sensei_lpwan.
list(APPEND EXTRA_ZEPHYR_MODULES $ENV{SENSEI_SDK_ROOT}/NRF)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_lpwan_shield)

target_sources(app PRIVATE
  src/main.c
  src/link.c
  src/service.c
)
target_include_directories(app PRIVATE src)

target_sources_ifdef(CONFIG_LPWAN_SHIELD_LTE app PRIVATE src/lte.c)

if(CONFIG_LPWAN_SHIELD_LORA)
  set(LR_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/lr1121_modem)
  target_sources(app PRIVATE
    src/lora.c
    src/lr1121_zephyr_hal.c
    ${LR_DIR}/lr1121_modem_bsp.c
    ${LR_DIR}/lr1121_modem_driver_version.c
    ${LR_DIR}/lr1121_modem_lorawan.c
    ${LR_DIR}/lr1121_modem_lr_fhss.c
    ${LR_DIR}/lr1121_modem_modem.c
    ${LR_DIR}/lr1121_modem_radio_timings.c
    ${LR_DIR}/lr1121_modem_radio.c
    ${LR_DIR}/lr1121_modem_regmem.c
    ${LR_DIR}/lr1121_modem_relay.c
    ${LR_DIR}/lr1121_modem_system.c
  )
  target_include_directories(app PRIVATE ${LR_DIR})
  # The Semtech driver is built as delivered; do not hold it to our warning level.
  set_source_files_properties(${LR_DIR}/lr1121_modem_lorawan.c ${LR_DIR}/lr1121_modem_radio.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
endif()
