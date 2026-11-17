/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/** @file pwr_ram.h
 *  @brief Power down the RAM the image does not use (nRF5340 application core, CONFIG_SENSEI_PWR_RAM_POWERDOWN).
 */

#ifndef SENSEI_PWR_RAM_H_
#define SENSEI_PWR_RAM_H_

#include <stddef.h>

/**
 * @brief Power down the RAM sections above the image that nothing uses.
 *
 * Range: from the end of the image RAM (_image_ram_end) to the end of the chosen zephyr,sram. Left alone, read
 * from the devicetree: every node with compatible "zephyr,memory-region" (linker regions, retained RAM) and the
 * parent of every "zephyr,retained-ram" node, and the chosen zephyr,ipc_shm (shared memory with the network core).
 * Whole 4 kB sections only. Static data, the stacks and the kernel heap are part of the image and stay.
 *
 * Call once, at the end of the start-up, from a thread. Never use malloc() afterwards: the Kconfig forbids a libc
 * arena that spans the free RAM. The nRF Connect SDK library powers all RAM on again before a software reboot
 * (CONFIG_NRF_FORCE_RAM_ON_REBOOT).
 *
 * @return bytes of RAM powered down
 */
size_t sensei_pwr_ram_powerdown(void);

#endif /* SENSEI_PWR_RAM_H_ */
