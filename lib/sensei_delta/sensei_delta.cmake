# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# For GAP9 SDK projects. The GAP SDK sets include paths, definitions and flags per application target
# (setupos), so the engine is compiled into that target instead of a separate library:
#   include(<sdk>/lib/sensei_delta/sensei_delta.cmake)
#   set(SENSEI_DELTA_PORT GAP9)          # optional: pi_perf profiling hooks
#   sensei_delta_add_sources(${TARGET_NAME})

get_filename_component(SENSEI_DELTA_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

function(sensei_delta_add_sources target)
    target_sources(${target} PRIVATE ${SENSEI_DELTA_DIR}/src/sensei_delta.c)
    target_include_directories(${target} PRIVATE ${SENSEI_DELTA_DIR}/include)
    # The codec-unit conversion must be plain IEEE double operations.
    target_compile_options(${target} PRIVATE -ffp-contract=off)
    if(SENSEI_DELTA_PORT)
        target_compile_definitions(${target} PRIVATE SENSEI_DELTA_PORT_${SENSEI_DELTA_PORT})
    endif()
    target_link_libraries(${target} PRIVATE m)
endfunction()
