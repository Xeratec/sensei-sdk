# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the link layer:
#   cmake -S lib/sensei_link/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_link_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_link)
target_compile_options(sensei_link PRIVATE -Wall -Wextra)

# The framing sources must build as strict C99 without warnings.
set_target_properties(sensei_link_framing PROPERTIES C_STANDARD 99)
target_compile_options(sensei_link_framing PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion)

enable_testing()
foreach(t test_framing test_pb)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE sensei_link)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# The committed generated files must match the .proto files. Needs NANOPB_GENERATOR (see scripts/generate.sh).
if(DEFINED ENV{NANOPB_GENERATOR})
  add_test(NAME generated_current COMMAND ${SENSEI_LINK_DIR}/scripts/generate.sh --check)
endif()
