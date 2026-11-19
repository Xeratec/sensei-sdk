# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the envelope library (needs OpenSSL 3; on macOS: -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)):
#   cmake -S lib/sensei_secure/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_secure_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_secure)
if(NOT SENSEI_SECURE_FOUND)
  return()
endif()
# Strict C99 with warnings as errors (the OpenSSL backend as well).
target_compile_options(sensei_secure PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(test_secure test_secure.c)
target_include_directories(test_secure PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
target_link_libraries(test_secure PRIVATE sensei_secure)
target_compile_options(test_secure PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_secure COMMAND test_secure)

add_executable(test_e2e test_e2e.c)
target_include_directories(test_e2e PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
target_link_libraries(test_e2e PRIVATE sensei_secure)
target_compile_options(test_e2e PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_e2e COMMAND test_e2e)

add_executable(test_seal test_seal.c)
target_include_directories(test_seal PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
target_link_libraries(test_seal PRIVATE sensei_secure)
target_compile_options(test_seal PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_seal COMMAND test_seal)

add_executable(test_ratchet test_ratchet.c)
target_include_directories(test_ratchet PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
target_link_libraries(test_ratchet PRIVATE sensei_secure)
target_compile_options(test_ratchet PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_ratchet COMMAND test_ratchet)

add_executable(test_commission test_commission.c)
target_include_directories(test_commission PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
target_link_libraries(test_commission PRIVATE sensei_secure)
target_compile_options(test_commission PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_commission COMMAND test_commission)
