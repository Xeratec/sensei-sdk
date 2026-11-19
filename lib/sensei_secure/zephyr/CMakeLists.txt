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
  ${SENSEI_SECURE_DIR}/src/sensei_ratchet.c
  ${SENSEI_SECURE_DIR}/src/sensei_e2e_seal.c
  ${SENSEI_SECURE_DIR}/src/sensei_secure_zephyr.c
)
zephyr_include_directories(${SENSEI_SECURE_DIR}/include)
# <crypto/sensei_crypto.h> of the SDK crypto library
zephyr_library_include_directories(${SENSEI_SECURE_DIR}/../../NRF/system)
zephyr_compile_definitions(SENSEI_SECURE_BACKEND_ZEPHYR)
zephyr_library_compile_definitions_ifdef(CONFIG_SENSEI_SECURE_RATCHET SENSEI_E2E_REKEY_STALE_EPOCHS=${CONFIG_SENSEI_SECURE_REKEY_STALE_EPOCHS}u)
zephyr_library_sources_ifdef(CONFIG_SENSEI_SECURE_SETTINGS ${CMAKE_CURRENT_LIST_DIR}/sensei_e2e_settings.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_SECURE_SETTINGS_SEAL_HUK ${CMAKE_CURRENT_LIST_DIR}/sensei_e2e_huk.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_SECURE_SETTINGS_SEAL_TEST_KEY ${CMAKE_CURRENT_LIST_DIR}/sensei_e2e_testkey.c)

if(CONFIG_SENSEI_SECURE_COMMISSION)
  zephyr_library_sources(${SENSEI_SECURE_DIR}/src/sensei_commission.c)
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR})
  if(CONFIG_SENSEI_SECURE_SPAKE2P_OCRYPTO)
    zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/sensei_spake2p_ocrypto.c)
    # ocrypto_spake2p_p256.h and the library (also linked for the Oberon PSA driver)
    if(TARGET mbedcrypto_oberon_imported)
      zephyr_library_link_libraries(mbedcrypto_oberon_imported)
    else()
      zephyr_library_include_directories(${ZEPHYR_NRFXLIB_MODULE_DIR}/crypto/nrf_oberon/include)
    endif()
  elseif(CONFIG_SENSEI_SECURE_SPAKE2P_MBEDTLS_ECP)
    zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/sensei_spake2p_mbedtls.c)
  else()
    message(FATAL_ERROR "CONFIG_SENSEI_SECURE_COMMISSION needs a SPAKE2+ backend (SENSEI_SECURE_SPAKE2P_OCRYPTO or _MBEDTLS_ECP)")
  endif()
  zephyr_library_sources_ifdef(CONFIG_SENSEI_SECURE_SETTINGS ${CMAKE_CURRENT_LIST_DIR}/sensei_commission_settings.c)
endif()
