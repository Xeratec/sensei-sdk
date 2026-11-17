/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include "ota/sensei_ota.h"

LOG_MODULE_REGISTER(sensei_ota, CONFIG_SENSEI_OTA_LOG_LEVEL);

#define CHECK_PERIOD_S 5

static sensei_ota_check_fn check_fn;
static struct k_work_delayable trial_work;

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
  int64_t up_s = k_uptime_get() / 1000;

  if (boot_is_img_confirmed()) { /* confirmed over SMP */
    LOG_INF("image confirmed by the host");
    return;
  }
  if (IS_ENABLED(CONFIG_SENSEI_OTA_CONFIRM) && up_s >= CONFIG_SENSEI_OTA_CONFIRM_DELAY_S &&
      (check_fn == NULL || check_fn())) {
    if (sensei_ota_confirm() == 0) {
      return;
    }
  }
  if (up_s >= CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S) {
    LOG_ERR("image not confirmed after %d s, resetting to revert", CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S);
    sys_reboot(SYS_REBOOT_COLD);
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
