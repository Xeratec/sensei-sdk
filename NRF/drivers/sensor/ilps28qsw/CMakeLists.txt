# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(ilps28qsw.c)
# The register library itself is compiled into the application by system/third_party.
zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/st/ilps28qsw)
