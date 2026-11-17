/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include "ota/sensei_ota.h"
#include "ota/sensei_ota_trial.h"

LOG_MODULE_REGISTER(sensei_ota, CONFIG_SENSEI_OTA_LOG_LEVEL);

#define CHECK_PERIOD_S 5

static sensei_ota_check_fn check_fn;
static struct k_work_delayable trial_work;
static atomic_t link_seen;

/* The SMP request hook is only called when mcumgr was built with the command status hooks. Without them the
 * link rule would never be met and every update would revert. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK) || IS_ENABLED(CONFIG_MCUMGR_SMP_COMMAND_STATUS_HOOKS),
             "SENSEI_OTA_CONFIRM_NEEDS_LINK needs CONFIG_MCUMGR_SMP_COMMAND_STATUS_HOOKS");

void sensei_ota_link_seen(void) { atomic_set(&link_seen, 1); }

/* Any SMP request, on any transport (Bluetooth, USB), shows that the image can still be updated. */
static enum mgmt_cb_return smp_request_cb(uint32_t event, enum mgmt_cb_return prev, int32_t *rc, uint16_t *group,
                                          bool *abort_more, void *data, size_t data_size) {
  ARG_UNUSED(event);
  ARG_UNUSED(prev);
  ARG_UNUSED(rc);
  ARG_UNUSED(group);
  ARG_UNUSED(abort_more);
  ARG_UNUSED(data);
  ARG_UNUSED(data_size);
  sensei_ota_link_seen();
  return MGMT_CB_OK;
}

static struct mgmt_callback smp_request_hook = {
    .callback = smp_request_cb,
    .event_id = MGMT_EVT_OP_CMD_RECV,
};

/* Register the hook at boot, not in sensei_ota_init() at the end of the app's main(): a request that comes earlier
 * (a host that is already connected) counts too. The hook does nothing on a confirmed image. */
static int smp_hook_init(void) {
  if (IS_ENABLED(CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK)) {
    mgmt_callback_register(&smp_request_hook);
  }
  return 0;
}

SYS_INIT(smp_hook_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

enum sensei_ota_state sensei_ota_state(void) {
  return boot_is_img_confirmed() ? SENSEI_OTA_CONFIRMED : SENSEI_OTA_TRIAL;
}

int sensei_ota_confirm(void) {
  int rc = boot_write_img_confirmed();

  if (rc == 0) {
    LOG_INF("image confirmed");
  } else {
    LOG_ERR("confirm failed: %d", rc);
  }
  return rc;
}

/* MCUboot image format: 32 B header, image, then the TLV area (magic 0x6907 or 0x6908 + total length, then
 * type, pad, length and value). The SHA-256 of the image is TLV 0x10. */
#define IMAGE_MAGIC 0x96f3b83dU
#define TLV_INFO_MAGIC 0x6907
#define TLV_PROT_INFO_MAGIC 0x6908
#define TLV_SHA256 0x10

uint32_t sensei_ota_image_id(void) {
  const struct flash_area *fa;
  uint8_t buf[32];
  uint32_t id = 0;

  if (flash_area_open(FIXED_PARTITION_ID(slot0_partition), &fa) != 0) {
    return 0;
  }
  if (flash_area_read(fa, 0, buf, sizeof(buf)) == 0 && sys_get_le32(buf) == IMAGE_MAGIC) {
    off_t pos = sys_get_le16(buf + 8) + (off_t)sys_get_le32(buf + 12); /* header + image */

    for (int area = 0; area < 2 && id == 0; area++) { /* protected TLVs, if any, then the rest */
      uint8_t hdr[4];

      if (flash_area_read(fa, pos, hdr, sizeof(hdr)) != 0) {
        break;
      }
      uint16_t magic = sys_get_le16(hdr), total = sys_get_le16(hdr + 2);
      off_t end = pos + total;

      if (magic != TLV_INFO_MAGIC && magic != TLV_PROT_INFO_MAGIC) {
        break;
      }
      for (off_t p = pos + 4; p + 4 <= end; p += 4 + sys_get_le16(hdr + 2)) {
        uint8_t v[4];

        if (flash_area_read(fa, p, hdr, sizeof(hdr)) != 0) {
          break;
        }
        if (hdr[0] == TLV_SHA256 && sys_get_le16(hdr + 2) == 32 && flash_area_read(fa, p + 4, v, sizeof(v)) == 0) {
          id = sys_get_le32(v);
          break;
        }
      }
      if (magic == TLV_INFO_MAGIC) {
        break;
      }
      pos = end;
    }
  }
  flash_area_close(fa);
  return id;
}

static void trial_fn(struct k_work *work) {
  if (boot_is_img_confirmed()) { /* confirmed over SMP */
    LOG_INF("image confirmed by the host");
    return;
  }
  struct sensei_ota_trial_in in = {
      .up_s = (uint32_t)(k_uptime_get() / 1000),
      .confirm_enabled = IS_ENABLED(CONFIG_SENSEI_OTA_CONFIRM),
      .confirm_delay_s = CONFIG_SENSEI_OTA_CONFIRM_DELAY_S,
      .timeout_s = CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S,
      .need_link = IS_ENABLED(CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK),
      .link_seen = atomic_get(&link_seen) != 0,
  };

  /* The check may start work (self-tests): call it once the delay is over, not before. */
  in.healthy = in.confirm_enabled && in.up_s >= in.confirm_delay_s && (check_fn == NULL || check_fn());
  switch (sensei_ota_trial_step(&in)) {
  case SENSEI_OTA_TRIAL_CONFIRM:
    if (sensei_ota_confirm() == 0) {
      return;
    }
    break;
  case SENSEI_OTA_TRIAL_REVERT:
    LOG_ERR("image not confirmed after %d s, resetting to revert", CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S);
    sys_reboot(SYS_REBOOT_COLD);
    break;
  case SENSEI_OTA_TRIAL_WAIT:
    break;
  }
  k_work_reschedule(&trial_work, K_SECONDS(CHECK_PERIOD_S));
}

int sensei_ota_init(sensei_ota_check_fn check) {
  check_fn = check;
  if (boot_is_img_confirmed()) {
    return 0;
  }
  LOG_WRN("image is on trial: confirm within %d s or it reverts", CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S);
  k_work_init_delayable(&trial_work, trial_fn);
  k_work_schedule(&trial_work, K_SECONDS(CHECK_PERIOD_S));
  return 0;
}
