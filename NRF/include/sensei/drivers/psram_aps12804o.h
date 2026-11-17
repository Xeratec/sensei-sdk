/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file psram_aps12804o.h
 * @brief APS12804O QSPI PSRAM (16 MiB) behind the nRF5340 QSPI peripheral. Untested on hardware.
 *
 * Enable the node in the devicetree (it is disabled by default):
 *   &psram { status = "okay"; };
 * Power states: on (about 64 uA standby), half-sleep (about 19.5 uA, data kept), off (load switch,
 * data lost, no current).
 */

#ifndef SENSEI_DRIVERS_PSRAM_APS12804O_H_
#define SENSEI_DRIVERS_PSRAM_APS12804O_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSRAM_APS12804O_SIZE (16U * 1024U * 1024U)

/** @brief Switch the supply on, select the PSRAM, start QSPI, reset the chip and check its ID. */
int psram_aps12804o_power_on(const struct device *dev);

/** @brief Read len bytes from addr. Any alignment. */
int psram_aps12804o_read(const struct device *dev, uint32_t addr, void *buf, size_t len);

/** @brief Write len bytes to addr. Any alignment. */
int psram_aps12804o_write(const struct device *dev, uint32_t addr, const void *buf, size_t len);

/** @brief Half-sleep: data is kept, QSPI is deactivated. Leave with psram_aps12804o_wake(). */
int psram_aps12804o_half_sleep(const struct device *dev);

/** @brief Leave half-sleep (takes 150 us). */
int psram_aps12804o_wake(const struct device *dev);

/** @brief Stop QSPI and switch the supply off. The content is lost. */
int psram_aps12804o_power_off(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_PSRAM_APS12804O_H_ */
