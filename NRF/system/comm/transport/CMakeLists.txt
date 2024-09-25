# Copyright (c) 2024 ETH Zurich
# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE
    transport.c
)

add_subdirectory(backends)
add_subdirectory(endpoints)
