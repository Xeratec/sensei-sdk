/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Refuse an SMP reset into the MCUboot recovery unless USB power is present (CONFIG_SENSEI_OTA_RECOVERY_NEEDS_USB). */

#include <zcbor_common.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>

#include "ota/sensei_ota.h"
#include "ota/sensei_ota_reset.h"

#ifdef CONFIG_SOC_NRF5340_CPUAPP
#include <hal/nrf_usbreg.h>
#include <nrfx.h>
#endif

LOG_MODULE_REGISTER(sensei_ota_reset, CONFIG_LOG_DEFAULT_LEVEL);

/* The hook sees the request, not the transport it came over: it cannot tell USB from Bluetooth. USB power is what a
 * person with a cable has, and the recovery needs the cable anyway. */
__weak bool sensei_ota_usb_power(void) {
#ifdef CONFIG_SOC_NRF5340_CPUAPP
  return (nrf_usbreg_status_get(NRF_USBREGULATOR) & NRF_USBREG_STATUS_VBUSDETECT_MASK) != 0;
#else
  return false;
#endif
}

static enum mgmt_cb_return reset_cb(uint32_t event, enum mgmt_cb_return prev, int32_t *rc, uint16_t *group,
                                    bool *abort_more, void *data, size_t data_size) {
  const struct os_mgmt_reset_data *d = data;

  ARG_UNUSED(event);
  ARG_UNUSED(prev);
  ARG_UNUSED(group);
  if (data_size < sizeof(*d) || sensei_ota_reset_allowed(d->boot_mode, sensei_ota_usb_power())) {
    return MGMT_CB_OK;
  }
  LOG_WRN("reset into boot mode %u refused: no USB power", (unsigned)d->boot_mode);
  *rc = MGMT_ERR_EACCESSDENIED;
  *abort_more = true;
  return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback reset_hook = {
    .callback = reset_cb,
    .event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

static int reset_hook_init(void) {
  mgmt_callback_register(&reset_hook);
  return 0;
}

SYS_INIT(reset_hook_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
