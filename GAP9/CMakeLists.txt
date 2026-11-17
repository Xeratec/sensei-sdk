# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

# SENSEI custom BSP for GAP SDK >= 5.22. The SDK includes this file from rtos/pmsis when the
# application sets GAP_CUSTOM_BSP (= GAP_CUSTOM_BSP_DIR). Sources go straight into the pmsis target.

set(ENV{CUSTOM_BSP_TARGET} pmsis)

# Board options
if(CONFIG_BOARD_SENSEI_BASEV1)
    list(APPEND CUSTOM_BSP_SRCS "${GAP_CUSTOM_BSP}/bsp/boards/bsp_sensei_basev1.c")
    list(APPEND CUSTOM_BSP_COMPILE_OPTIONS "-DCONFIG_BOARD_SENSEI_BASEV1" "-D__GAP_BOARD_SENSEI_BASEV1__=1")
endif()

if(CONFIG_BOARD_SENSEI_SENSORV1)
    list(APPEND CUSTOM_BSP_SRCS "${GAP_CUSTOM_BSP}/bsp/boards/bsp_sensei_sensorv1.c")
    list(APPEND CUSTOM_BSP_COMPILE_OPTIONS "-DCONFIG_BOARD_SENSEI_SENSORV1" "-D__GAP_BOARD_SENSEI_SENSORV1__=1")
endif()

if(CONFIG_BOARD_SENSEI_DEBUGV1)
    list(APPEND CUSTOM_BSP_SRCS "${GAP_CUSTOM_BSP}/bsp/boards/bsp_sensei_debugv1.c")
    list(APPEND CUSTOM_BSP_COMPILE_OPTIONS "-DCONFIG_BOARD_SENSEI_DEBUGV1" "-D__GAP_BOARD_SENSEI_DEBUGV1__=1")
endif()

if(CUSTOM_BSP_SRCS)
    message(STATUS "${CUSTOM_BSP_PREFIX} Sources: ${CUSTOM_BSP_SRCS}")
    target_sources($ENV{CUSTOM_BSP_TARGET} PRIVATE ${CUSTOM_BSP_SRCS})
    target_include_directories($ENV{CUSTOM_BSP_TARGET} PUBLIC ${GAP_CUSTOM_BSP}/bsp/boards)
    target_compile_options($ENV{CUSTOM_BSP_TARGET} PUBLIC ${CUSTOM_BSP_COMPILE_OPTIONS})
endif()

# Note: the SDK's bsp/bsp.h only includes its own board headers (bsp_boards.h). Applications that call
# bsp_aps256xxn_open() and friends include "bsp_sensei_basev1.h" (bsp/boards is on the include path).
