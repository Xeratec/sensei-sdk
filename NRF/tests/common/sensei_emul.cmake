# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# The I2C sensor emulators of the driver tests, for any app on native_sim (an application of another repository, for
# example a simulated node). The emulators stay in their test folders; this file adds them to a target.
#
#   include($ENV{SENSEI_SDK_ROOT}/NRF/tests/common/sensei_emul.cmake)   # after find_package(Zephyr)
#   sensei_emul_sources(app scd41 sgp41 bh1730)
#
# The devicetree needs the node of the sensor (compatible "sensei,scd41", "sensirion,sgp41", "sensei,bh1730") on an
# emulated I2C bus (CONFIG_I2C_EMUL, CONFIG_EMUL); SCD41 and SGP41 also the GPIOs of their rails (see the app.overlay
# of the test). The test controls are in tests/<name>/src/emul_<name>.h: struct <name>_emul_get() holds the values
# that the sensor answers.

set(SENSEI_EMUL_DIR ${CMAKE_CURRENT_LIST_DIR}/.. CACHE INTERNAL "")

function(sensei_emul_sources target)
  foreach(name ${ARGN})
    if(NOT EXISTS ${SENSEI_EMUL_DIR}/${name}/src/emul_${name}.c)
      message(FATAL_ERROR "sensei_emul_sources: no emulator '${name}' (scd41, sgp41, bh1730)")
    endif()
    target_sources(${target} PRIVATE ${SENSEI_EMUL_DIR}/${name}/src/emul_${name}.c)
    target_include_directories(${target} PRIVATE ${SENSEI_EMUL_DIR}/${name}/src)
  endforeach()
  target_include_directories(${target} PRIVATE ${SENSEI_EMUL_DIR}/common)
endfunction()
