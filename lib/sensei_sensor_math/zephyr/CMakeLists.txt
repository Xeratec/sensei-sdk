# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_SENSOR_MATH is set.
set(SENSEI_SENSOR_MATH_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_sensor_math)
zephyr_library_sources(${SENSEI_SENSOR_MATH_DIR}/src/sensei_sensor_math.c)
zephyr_include_directories(${SENSEI_SENSOR_MATH_DIR}/include)
