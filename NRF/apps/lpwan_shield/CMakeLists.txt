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
  src/diag.c
  src/service.c
)
target_include_directories(app PRIVATE src)

if(CONFIG_LPWAN_SHIELD_SIM)
  # native_sim: the link is a socket, the modem and the LR1121 are fakes (sim/README.md). The headers of the modem
  # library, lte_lc and date_time are the real ones of NCS: the fakes must have their signatures.
  target_sources(app PRIVATE sim/sim_shield.c sim/sim_link.c)
  target_include_directories(app PRIVATE sim ${ZEPHYR_BASE}/boards/native/native_sim
    ${ZEPHYR_NRF_MODULE_DIR}/include ${ZEPHYR_NRFXLIB_MODULE_DIR}/nrf_modem/include)
  # what the Kconfig of the modem libraries would define on the board, and lte.c asks for
  target_compile_definitions(app PRIVATE
    CONFIG_LTE_LC_NETWORK_REGISTRATION_MODULE=1 CONFIG_LTE_LC_PSM_MODULE=1 CONFIG_LTE_LC_EDRX_MODULE=1
    CONFIG_LTE_LC_RAI_MODULE=1 CONFIG_LTE_LC_CONN_EVAL_MODULE=1 CONFIG_LTE_PROPRIETARY_PSM_REQ=1)
else()
  target_sources(app PRIVATE src/link.c)
endif()
# The trial rule and the image TLVs of the OTA helpers (pure headers, no Zephyr dependency): "ota/sensei_ota_trial.h",
# "ota/sensei_ota_image.h".
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../system)

target_sources_ifdef(CONFIG_LPWAN_SHIELD_DFU app PRIVATE src/dfu.c)

if(CONFIG_LPWAN_SHIELD_DFU)
  # The kind of this board: sysbuild.cmake adds it to the signed image as a protected TLV (imgtool --custom-tlv 0xa0),
  # and the app takes the string from the same imgtool arguments, so the two cannot differ.
  if(CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS MATCHES "--custom-tlv 0x[aA]0 ([^ ]+)")
    target_compile_definitions(app PRIVATE LPWAN_SHIELD_IMAGE_KIND="${CMAKE_MATCH_1}")
  else()
    message(FATAL_ERROR "The image kind TLV is missing in CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS "
                        "('${CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS}'): build with sysbuild (sysbuild.cmake sets it).")
  endif()
endif()

target_sources_ifdef(CONFIG_LPWAN_SHIELD_LTE app PRIVATE src/lte.c)
if(CONFIG_LPWAN_SHIELD_SIM AND CONFIG_LPWAN_SHIELD_LTE)
  target_sources(app PRIVATE sim/sim_modem.c)
endif()

if(CONFIG_LPWAN_SHIELD_LORA)
  set(LR_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/lr1121_modem)
  target_sources(app PRIVATE src/lora.c)
  target_include_directories(app PRIVATE ${LR_DIR})
  if(CONFIG_LPWAN_SHIELD_SIM)
    # the driver calls that lora.c makes are faked (the headers are the driver's)
    target_sources(app PRIVATE sim/sim_lr1121.c)
  else()
    target_sources(app PRIVATE
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
    # The Semtech driver is built as delivered; do not hold it to our warning level.
    set_source_files_properties(${LR_DIR}/lr1121_modem_lorawan.c ${LR_DIR}/lr1121_modem_radio.c
      PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
  endif()
endif()
