# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START of the NCS depends on SOC_NRF5340_CPUNET, which the nrf5340bsim network core
# does not set (it sets SOC_COMPATIBLE_NRF5340_CPUNET), so Kconfig refuses it. The MPSL library of nrfxlib for BabbleSim
# has the function behind it (mpsl_clock_task_trigger_on_rtc_start_set), so the macros are defined here for the sources.
# The channel is that of sysbuild/ipc_radio.conf (CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL).
zephyr_compile_definitions(CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START=1 CONFIG_MPSL_TRIGGER_IPC_TASK_ON_RTC_START_CHANNEL=4)
