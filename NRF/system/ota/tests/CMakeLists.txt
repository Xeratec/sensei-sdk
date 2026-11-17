# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the OTA helpers (the trial decision, the image TLVs):
#   cmake -S NRF/system/ota/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_ota_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()
add_executable(test_ota_trial test_ota_trial.c)
target_include_directories(test_ota_trial PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_options(test_ota_trial PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_trial COMMAND test_ota_trial)

add_executable(test_ota_image test_ota_image.c)
target_include_directories(test_ota_image PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_options(test_ota_image PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_image COMMAND test_ota_image)

add_executable(test_ota_reset test_ota_reset.c)
target_include_directories(test_ota_reset PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_options(test_ota_reset PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_reset COMMAND test_ota_reset)

add_executable(test_gap9_trial test_gap9_trial.c)
target_include_directories(test_gap9_trial PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_options(test_gap9_trial PRIVATE -Wall -Wextra -Werror)
add_test(NAME gap9_trial COMMAND test_gap9_trial)
