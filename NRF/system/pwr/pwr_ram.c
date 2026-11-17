/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "pwr/pwr_ram.h"

#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <ram_pwrdn.h>

/* nRF5340 application core: 8 banks of 64 kB, power switch per 4 kB section (as the nRF Connect SDK library) */
#define RAM_START 0x20000000UL
#define RAM_END (RAM_START + 8UL * 0x10000UL)
#define SECTION 0x1000UL

#if DT_HAS_CHOSEN(zephyr_sram)
#define SRAM_END (DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) + DT_REG_SIZE(DT_CHOSEN(zephyr_sram)))
#else
#define SRAM_END RAM_END
#endif

extern char _image_ram_end[];

/*
 * power_down_unused_ram() of the nRF Connect SDK powers down every whole section from the end of the image RAM to the
 * end of the chosen zephyr,sram. It knows no other region, so each one must lie above that range: the memory regions
 * of the devicetree (linker regions, the retained RAM), the retained RAM and the shared memory with the network core.
 * The boards put the retained RAM and the shared memory after the image RAM (nrf5340_senseiv1_retained.dtsi). A board
 * that does not must not use this module.
 */
#define ABOVE_SRAM(n)                                                                                                  \
  COND_CODE_1(DT_NODE_HAS_PROP(n, reg),                                                                                \
              (BUILD_ASSERT(DT_REG_ADDR(n) >= SRAM_END, "a memory region lies inside the RAM that is powered down");), \
              ())
#define ABOVE_SRAM_PARENT(n) ABOVE_SRAM(DT_PARENT(n))

DT_FOREACH_STATUS_OKAY(zephyr_memory_region, ABOVE_SRAM)
DT_FOREACH_STATUS_OKAY(zephyr_retained_ram, ABOVE_SRAM_PARENT)
#if DT_HAS_CHOSEN(zephyr_ipc_shm)
ABOVE_SRAM(DT_CHOSEN(zephyr_ipc_shm))
#endif

size_t sensei_pwr_ram_powerdown(void) {
  uintptr_t first = ROUND_UP((uintptr_t)_image_ram_end, SECTION);
  uintptr_t last = ROUND_DOWN(MIN((uintptr_t)SRAM_END, (uintptr_t)RAM_END), SECTION);

  power_down_unused_ram();
  return last > first ? last - first : 0;
}
