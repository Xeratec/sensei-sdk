# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

# Board options
    if(CONFIG_BOARD_SENSEI_BASEV1)
         list(APPEND CUSTOM_BSP_SRCS "${GAP_CUSTOM_BSP}/bsp/boards/bsp_sensei_basev1.c")
         list(APPEND CUSTOM_BSP_COMPILE_OPTIONS "-D__GAP_BOARD_SENSEI_BASEV1__=1")
    endif()

    if(CONFIG_BOARD_SENSEI_SENSORV1)
        list(APPEND CUSTOM_BSP_SRCS "${GAP_CUSTOM_BSP}/bsp/boards/bsp_sensei_sensorv1.c")
        list(APPEND CUSTOM_BSP_COMPILE_OPTIONS "-D__GAP_BOARD_SENSEI_SENSORV1__=1")
   endif()


    if(CONFIG_BOARD_SENSEI_DEBUGV1)
        list(APPEND CUSTOM_BSP_SRCS "${GAP_CUSTOM_BSP}/bsp/boards/bsp_sensei_debugv1.c")
        list(APPEND CUSTOM_BSP_COMPILE_OPTIONS "-D__GAP_BOARD_SENSEI_DEBUGV1__=1")
    endif()

target_include_directories(pmsis PUBLIC ${GAP_CUSTOM_BSP}/bsp/drivers)
target_include_directories(pmsis PUBLIC ${GAP_CUSTOM_BSP}/bsp/boards)

message(STATUS "Custom BSP sources: ${CUSTOM_BSP_SRCS}")
message(STATUS "Custom BSP compile options: ${CUSTOM_BSP_COMPILE_OPTIONS}")
message(STATUS "Custom BSP include directories: ${GAP_CUSTOM_BSP}/bsp/boards ${GAP_CUSTOM_BSP}/bsp/drivers")

# Driver options
if(CUSTOM_BSP_SRCS)
    target_sources(pmsis PRIVATE ${CUSTOM_BSP_SRCS})
    target_compile_options(pmsis PUBLIC ${CUSTOM_BSP_COMPILE_OPTIONS})
endif()