# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host test of the GAP9 link state machine (nRF5340 side) against a simulated GAP9 master:
#   cmake -S lib/sensei_gap9/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_gap9_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_gap9)
target_compile_options(sensei_gap9_core PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(test_gap9_core test_gap9_core.c)
target_link_libraries(test_gap9_core PRIVATE sensei_gap9_core)
target_compile_options(test_gap9_core PRIVATE -Wall -Wextra -Werror)
add_test(NAME gap9_core COMMAND test_gap9_core)

# Slot format and checks of the SSBL (header only, no link library).
add_executable(test_gap9_slot test_gap9_slot.c)
target_include_directories(test_gap9_slot PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_options(test_gap9_slot PRIVATE -Wall -Wextra -Werror)
add_test(NAME gap9_slot COMMAND test_gap9_slot)

# Trial rule of a GAP9 slot (header only).
add_executable(test_gap9_trial test_gap9_trial.c)
target_include_directories(test_gap9_trial PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include)
target_compile_options(test_gap9_trial PRIVATE -Wall -Wextra -Werror)
add_test(NAME gap9_trial COMMAND test_gap9_trial)

# The writer of the GAP9 update on a RAM "MRAM": transfer, resume, a power cut at every write, the slot that runs
# never written. With an image file as argument it feeds that file through the writer (the cross-check with the
# image builder, GAP9/ssbl/tools/test_pack_slot.py).
add_executable(test_g9ota test_g9ota.c)
target_link_libraries(test_g9ota PRIVATE sensei_gap9_ota)
target_compile_options(test_g9ota PRIVATE -Wall -Wextra -Werror)
add_test(NAME g9ota COMMAND test_g9ota)

# The update flow of the nRF5340 against a fake GAP9 link whose GAP9 runs the real writer on a RAM MRAM: transfer,
# signature check, trial boot, revert, reset in the middle of a trial, lost settings.
add_executable(test_g9ota_flow test_g9ota_flow.c)
target_link_libraries(test_g9ota_flow PRIVATE sensei_gap9_ota)
target_compile_options(test_g9ota_flow PRIVATE -Wall -Wextra -Werror)
add_test(NAME g9ota_flow COMMAND test_g9ota_flow)
