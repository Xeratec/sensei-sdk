/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gnss_m10s.h
 * @brief Extras of the MAX-M10S I2C GNSS driver beyond the Zephyr GNSS API.
 *
 * Fix data and satellites come through the standard API (GNSS_DATA_CALLBACK_DEFINE,
 * GNSS_SATELLITES_CALLBACK_DEFINE). The lowest power state is software standby: suspend the device
 * with pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND) (needs CONFIG_PM_DEVICE) or call
 * gnss_m10s_standby(). Wake it with gnss_m10s_wake() or PM_DEVICE_ACTION_RESUME.
 */

#ifndef SENSEI_DRIVERS_GNSS_M10S_H_
#define SENSEI_DRIVERS_GNSS_M10S_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Receiver power save modes. */
enum gnss_m10s_power_save {
  GNSS_M10S_PSM_FULL = 0,   /**< full power */
  GNSS_M10S_PSM_ON_OFF = 1, /**< PSMOO: one fix per period, then sleep */
  GNSS_M10S_PSM_CYCLIC = 2, /**< PSMCT: cyclic tracking */
};

/**
 * @brief Set the receiver power save mode.
 * @param period_s position update period for ON_OFF and CYCLIC (at least 5)
 */
int gnss_m10s_set_power_save(const struct device *dev, enum gnss_m10s_power_save mode, uint32_t period_s);

/**
 * @brief Ask for one NAV-PVT now (and NAV-SAT with CONFIG_GNSS_SATELLITES). Works in both modes.
 */
int gnss_m10s_poll(const struct device *dev);

/**
 * @brief Put the receiver into software standby (UBX-RXM-PMREQ with backup, wake on EXTINT).
 * RAM, RTC and the configuration in the BBR layer are kept. Needs the 3V3 rail to stay on.
 */
int gnss_m10s_standby(const struct device *dev);

/** @brief Wake the receiver from standby with an EXTINT edge (or a reset if there is no EXTINT pin). */
int gnss_m10s_wake(const struct device *dev);

/** @brief Hardware reset through RESET_N. Needs reset-gpios. */
int gnss_m10s_reset(const struct device *dev);

/**
 * @brief Share the I2C bus with other users (the GAP9 camera owns it while it captures).
 *
 * The polling work calls lock() before it touches the bus and unlock() after. lock() must not block: return 0
 * when the bus is yours, or -EBUSY when it is not. On -EBUSY the driver skips the round and looks again after
 * 100 ms. Pass NULL to remove the hooks. The calls made by the application (configure, standby, wake,
 * reset, power save) do not use the hooks: take your own lock around them.
 */
void gnss_m10s_set_bus_hooks(const struct device *dev, int (*lock)(void *ctx), void (*unlock)(void *ctx), void *ctx);

/** What the Zephyr GNSS API does not carry from the last UBX-NAV-PVT. */
struct gnss_m10s_pvt_ext {
  uint32_t itow_ms;  /**< GPS time of week of the fix, ms (to match it with a published fix) */
  uint32_t h_acc_mm; /**< horizontal accuracy estimate */
  uint32_t v_acc_mm; /**< vertical accuracy estimate */
  int32_t height_mm; /**< height above the WGS84 ellipsoid */
  uint16_t p_dop_e2; /**< position DOP, 0.01 */
  uint8_t fix_type;  /**< UBX fixType: 2 = 2D, 3 = 3D */
  uint8_t num_sv;    /**< satellites used */
  bool fix_ok;       /**< gnssFixOK flag set and the position valid */
  bool valid;        /**< a NAV-PVT has arrived since boot */
};

/**
 * @brief Copy the extras of the last NAV-PVT (thread and callback safe).
 *
 * The driver stores them just before it publishes the fix. Called from inside a GNSS_DATA_CALLBACK they belong to
 * the fix being published. From elsewhere they are those of the latest fix, which may be newer than the one seen.
 * @retval 0, -EAGAIN if no NAV-PVT has arrived yet (out is zero)
 */
int gnss_m10s_get_pvt_ext(const struct device *dev, struct gnss_m10s_pvt_ext *out);

/** @brief True if the module answered and accepted the configuration. */
bool gnss_m10s_is_configured(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_GNSS_M10S_H_ */
