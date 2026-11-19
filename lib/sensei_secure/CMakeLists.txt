# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds with the OpenSSL 3 backend (the Zephyr library is in zephyr/):
#   add_subdirectory(<sdk>/lib/sensei_secure sensei_secure)
#   target_link_libraries(<target> PRIVATE sensei_secure)
# Without OpenSSL 3 no target is made and SENSEI_SECURE_FOUND is OFF in the parent scope. On macOS
# Homebrew's OpenSSL is used (or pass -DOPENSSL_ROOT_DIR=<prefix>).
# Host tests: cmake -S <sdk>/lib/sensei_secure/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

if(NOT OPENSSL_ROOT_DIR AND APPLE)
  execute_process(COMMAND brew --prefix openssl@3 OUTPUT_VARIABLE _brew_ssl OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _brew_rc)
  if(_brew_rc EQUAL 0)
    set(OPENSSL_ROOT_DIR ${_brew_ssl})
  endif()
endif()
find_package(OpenSSL 3 QUIET)
if(NOT OpenSSL_FOUND)
  message(WARNING "OpenSSL 3 not found: sensei_secure is not built (-DOPENSSL_ROOT_DIR=<prefix>)")
  set(SENSEI_SECURE_FOUND OFF PARENT_SCOPE)
  return()
endif()
set(SENSEI_SECURE_FOUND ON PARENT_SCOPE)

add_library(sensei_secure STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/sensei_secure.c
  ${CMAKE_CURRENT_LIST_DIR}/src/sensei_ratchet.c
  ${CMAKE_CURRENT_LIST_DIR}/src/sensei_commission.c
  ${CMAKE_CURRENT_LIST_DIR}/src/sensei_e2e.c
  ${CMAKE_CURRENT_LIST_DIR}/src/sensei_e2e_seal.c
  ${CMAKE_CURRENT_LIST_DIR}/src/sensei_secure_openssl.c)
target_include_directories(sensei_secure PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_definitions(sensei_secure PUBLIC SENSEI_SECURE_BACKEND_OPENSSL)
target_link_libraries(sensei_secure PUBLIC OpenSSL::Crypto)
