/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * The image id (TLV walk) follows NRF/system/ota/sensei_ota.c.
 */

/*
 * App target: the signed image (TF-M and app, one MCUboot image) is written into slot 1 of the flash map
 * (`slot1_partition`, 448 kB at 0x80000). The checkpoints of the transfer are in the first sector of the
 * `storage_partition` (TF-M NS storage is on: the sector is non-secure). `dfu_apply` marks slot 1 for one trial boot;
 * the nRF5340 then cuts the shield power and powers it again, MCUboot swaps and starts the new image, which runs on
 * trial until the nRF5340 accepts the handshake (dfu_confirm). Any reset before that brings the old image back.
 *
 * Modem target: nrf_modem_delta_dfu. The delta patch goes to the modem in the order it arrives; the modem erases its
 * DFU area first (minutes), then takes the patch and checks its signature itself. The update runs in the modem at the
 * next start of the modem library: a worker shuts the library down and starts it again.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_LPWAN_SHIELD_MODEM_DFU
#include <modem/nrf_modem_lib.h>
#include <nrf_errno.h>
#include <nrf_modem.h>
#include <nrf_modem_delta_dfu.h>
#endif

#include "dfu.h"
#include "link.h"
#include "lte.h"
#include "ota/sensei_ota_trial.h"
#include "sensei_lpwan/lpwan_dfu.h"

LOG_MODULE_REGISTER(dfu, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

#define SLOT0_ID PARTITION_ID(slot0_partition)
#define SLOT1_ID PARTITION_ID(slot1_partition)
#define STORE_ID PARTITION_ID(storage_partition)

/* The delta patches are far below this; at the start the modem refuses a patch that does not fit its DFU area. */
#define MODEM_PATCH_MAX (2u * 1024u * 1024u)

static struct lpwan_dfu core;
static const struct flash_area *slot1;
static const struct flash_area *store;

/* ---- app: slot 1 and the checkpoint sector ---- */

static int open_areas(void) {
  int err;

  if (slot1 == NULL) {
    err = flash_area_open(SLOT1_ID, &slot1);
    if (err) {
      slot1 = NULL;
      return err;
    }
  }
  if (store == NULL) {
    err = flash_area_open(STORE_ID, &store);
    if (err) {
      store = NULL; /* no checkpoints: the transfer runs, a restart of the shield starts it again at 0 */
    }
  }
  return 0;
}

static uint32_t app_capacity(void *ctx, sensei_DfuTarget t) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  return open_areas() == 0 ? (uint32_t)slot1->fa_size : 0;
}

static int app_open(void *ctx, sensei_DfuTarget t, uint32_t size, bool fresh) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  ARG_UNUSED(size);
  ARG_UNUSED(fresh);
  return open_areas();
}

static int app_erase(void *ctx, uint32_t off, uint32_t len) {
  ARG_UNUSED(ctx);
  return flash_area_erase(slot1, off, len);
}

static int app_write(void *ctx, uint32_t off, const uint8_t *data, size_t len) {
  ARG_UNUSED(ctx);
  return flash_area_write(slot1, off, data, len);
}

static int app_read(void *ctx, uint32_t off, uint8_t *data, size_t len) {
  ARG_UNUSED(ctx);
  return flash_area_read(slot1, off, data, len);
}

static int log_read(void *ctx, uint32_t off, uint8_t *data, size_t len) {
  ARG_UNUSED(ctx);
  return store != NULL ? flash_area_read(store, off, data, len) : -ENODEV;
}

static int log_write(void *ctx, uint32_t off, const uint8_t *data, size_t len) {
  ARG_UNUSED(ctx);
  return store != NULL ? flash_area_write(store, off, data, len) : -ENODEV;
}

static int log_erase(void *ctx) {
  ARG_UNUSED(ctx);
  return store != NULL ? flash_area_erase(store, 0, LPWAN_DFU_SECTOR) : -ENODEV;
}

static int app_headers(void *ctx, uint8_t new_hdr[32], uint8_t running_hdr[32]) {
  const struct flash_area *s0;
  int err;

  ARG_UNUSED(ctx);
  err = flash_area_read(slot1, 0, new_hdr, 32);
  if (err) {
    return err;
  }
  err = flash_area_open(SLOT0_ID, &s0);
  if (err) {
    return err;
  }
  err = flash_area_read(s0, 0, running_hdr, 32);
  flash_area_close(s0);
  return err;
}

static int app_apply(void *ctx, sensei_DfuTarget t) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  /* One test boot: MCUboot swaps, and goes back at the next reset unless the new image confirms itself. */
  return boot_request_upgrade(BOOT_UPGRADE_TEST);
}

static void kick(void *ctx) {
  ARG_UNUSED(ctx);
  link_feed_watchdog();
}

static const struct lpwan_dfu_ops app_ops = {
    .capacity = app_capacity,
    .open = app_open,
    .erase = app_erase,
    .write = app_write,
    .read = app_read,
    .log_read = log_read,
    .log_write = log_write,
    .log_erase = log_erase,
    .read_headers = app_headers,
    .apply = app_apply,
    .kick = kick,
};

/* ---- modem ---- */

#ifdef CONFIG_LPWAN_SHIELD_MODEM_DFU

static bool erase_pending;
static bool write_open;
static atomic_t apply_result; /* 0 running, 1 done, negative: failed (detail in apply_detail) */
static int apply_detail;
static struct k_work apply_work;

static int modem_open(uint32_t size, bool fresh) {
  struct nrf_modem_delta_dfu_uuid uuid;
  size_t area = 0, off = 0;
  int err;

  err = lte_modem_init();
  if (err) {
    return err;
  }
  err = nrf_modem_delta_dfu_uuid(&uuid);
  if (err == 0) {
    LOG_INF("modem firmware %.*s", (int)sizeof(uuid.data), uuid.data);
  }
  err = nrf_modem_delta_dfu_area(&area);
  if (err) {
    return err > 0 ? err : -EIO;
  }
  if (size > area) {
    LOG_ERR("patch of %u B does not fit the modem DFU area (%u B)", (unsigned)size, (unsigned)area);
    return -EFBIG;
  }
  err = nrf_modem_delta_dfu_offset(&off);
  if (err != 0 && err != NRF_MODEM_DELTA_DFU_ERASE_PENDING) {
    return err > 0 ? err : -EIO;
  }
  erase_pending = err == NRF_MODEM_DELTA_DFU_ERASE_PENDING;
  if (fresh && !erase_pending && off != 0) { /* data of an earlier transfer, or a dirty area: erase it */
    err = nrf_modem_delta_dfu_erase();
    if (err != 0 && err != NRF_MODEM_DELTA_DFU_ERASE_PENDING) {
      return err > 0 ? err : -EIO;
    }
    erase_pending = true;
  }
  return 0;
}

/* > 0 while the modem erases its DFU area (it can take minutes), 0 ready, < 0 an error. */
static int modem_busy(void) {
  size_t off;
  int err;

  if (!erase_pending) {
    return 0;
  }
  err = nrf_modem_delta_dfu_offset(&off);
  if (err == 0) {
    erase_pending = false;
    return 0;
  }
  if (err == NRF_MODEM_DELTA_DFU_ERASE_PENDING || err == NRF_MODEM_DELTA_DFU_INVALID_DATA) {
    return 1;
  }
  return err > 0 ? -err : err;
}

static int modem_write(const uint8_t *data, size_t len) {
  int err;

  if (!write_open) {
    err = nrf_modem_delta_dfu_write_init();
    if (err != 0 && err != -NRF_EALREADY) {
      return err > 0 ? err : -EIO;
    }
    write_open = true;
  }
  err = nrf_modem_delta_dfu_write(data, len);
  return err; /* 0, a negative library error or a positive modem error: the core reports it in `detail` */
}

static int modem_finish(void) {
  int err = 0;

  if (write_open) {
    err = nrf_modem_delta_dfu_write_done();
    write_open = false;
  }
  return err == 0 ? 0 : (err > 0 ? err : -EIO);
}

static int dfu_result;
NRF_MODEM_LIB_ON_DFU_RES(sensei_dfu, on_dfu_res, NULL);
static void on_dfu_res(int dfu_res, void *ctx) {
  ARG_UNUSED(ctx);
  dfu_result = dfu_res;
}

/* The update runs when the modem library starts: shut it down and start it again. nrf_modem_lib_init() can take
 * minutes during an update. */
static void apply_fn(struct k_work *w) {
  int err;

  ARG_UNUSED(w);
  dfu_result = 0;
  err = nrf_modem_lib_shutdown();
  if (err == 0) {
    err = nrf_modem_lib_init();
  }
  lte_modem_restarted(err == 0);
  if (err == 0 && (dfu_result == 0 || dfu_result == (int)NRF_MODEM_DFU_RESULT_OK)) {
    apply_detail = 0;
    atomic_set(&apply_result, 1);
    LOG_INF("modem firmware updated");
  } else {
    apply_detail = dfu_result != 0 ? dfu_result : err;
    atomic_set(&apply_result, -1);
    LOG_ERR("modem update failed: init %d, result 0x%x", err, (unsigned)dfu_result);
  }
}

static int modem_apply(void) {
  int err = nrf_modem_delta_dfu_update();

  if (err != 0) {
    return err > 0 ? err : -EIO;
  }
  atomic_set(&apply_result, 0);
  k_work_init(&apply_work, apply_fn);
  k_work_submit(&apply_work); /* system work queue: the link thread stays free to answer status requests */
  return 0;
}

static int modem_apply_state(int *detail) {
  int r = (int)atomic_get(&apply_result);

  *detail = apply_detail;
  return r;
}

static uint32_t modem_capacity(void *ctx, sensei_DfuTarget t) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  return MODEM_PATCH_MAX;
}

static int modem_op_open(void *ctx, sensei_DfuTarget t, uint32_t size, bool fresh) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  return modem_open(size, fresh);
}

static int modem_op_busy(void *ctx) {
  ARG_UNUSED(ctx);
  return modem_busy();
}

static int modem_op_write(void *ctx, uint32_t off, const uint8_t *data, size_t len) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(off); /* the modem takes the patch in order */
  return modem_write(data, len);
}

static int modem_op_finish(void *ctx, sensei_DfuTarget t) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  return modem_finish();
}

static int modem_op_apply(void *ctx, sensei_DfuTarget t) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  return modem_apply();
}

static int modem_op_state(void *ctx, int *detail) {
  ARG_UNUSED(ctx);
  return modem_apply_state(detail);
}

static const struct lpwan_dfu_ops modem_ops = {
    .capacity = modem_capacity,
    .open = modem_op_open,
    .busy = modem_op_busy,
    .write = modem_op_write,
    .finish = modem_op_finish,
    .apply = modem_op_apply,
    .apply_state = modem_op_state,
    .kick = kick,
};

#endif /* CONFIG_LPWAN_SHIELD_MODEM_DFU */

/* ---- image id and trial boot ---- */

/* MCUboot image format: 32 B header, image, then the TLV area (magic 0x6907 or 0x6908 + total length, then
 * type, pad, length and value). The SHA-256 of the image is TLV 0x10. */
#define IMAGE_MAGIC 0x96f3b83dU
#define TLV_INFO_MAGIC 0x6907
#define TLV_PROT_INFO_MAGIC 0x6908
#define TLV_SHA256 0x10

static uint32_t running_image_id(void) {
  const struct flash_area *fa;
  uint8_t buf[32];
  uint32_t id = 0;

  if (flash_area_open(SLOT0_ID, &fa) != 0) {
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

static atomic_t handshake_ok; /* the nRF5340 accepted this image (dfu_confirm) */
static struct k_work_delayable trial_work;

/* Called by the check every few seconds while the image is on trial. */
static void trial_fn(struct k_work *work) {
  struct sensei_ota_trial_in in = {
      .up_s = (uint32_t)(k_uptime_get() / 1000),
      .confirm_enabled = true,
      .confirm_delay_s = CONFIG_LPWAN_SHIELD_CONFIRM_DELAY_S,
      .timeout_s = CONFIG_LPWAN_SHIELD_TRIAL_TIMEOUT_S,
      .need_link = true,
      .link_seen = atomic_get(&handshake_ok) != 0,
      .healthy = true, /* the link thread answers: the nRF5340's request got here */
  };

  ARG_UNUSED(work);
  if (boot_is_img_confirmed()) {
    return;
  }
  switch (sensei_ota_trial_step(&in)) {
  case SENSEI_OTA_TRIAL_CONFIRM:
    if (boot_write_img_confirmed() == 0) {
      LOG_INF("image confirmed");
      return;
    }
    LOG_ERR("confirm failed");
    break;
  case SENSEI_OTA_TRIAL_REVERT:
    LOG_ERR("image not confirmed after %d s: reset, MCUboot goes back to the old image",
            CONFIG_LPWAN_SHIELD_TRIAL_TIMEOUT_S);
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
    break;
  case SENSEI_OTA_TRIAL_WAIT:
    break;
  }
  k_work_reschedule(&trial_work, K_SECONDS(5));
}

void dfu_init(void) {
#ifdef CONFIG_LPWAN_SHIELD_MODEM_DFU
  lpwan_dfu_init(&core, &app_ops, &modem_ops, NULL);
#else
  lpwan_dfu_init(&core, &app_ops, NULL, NULL);
#endif
  if (boot_is_img_confirmed()) {
    return;
  }
  LOG_WRN("image is on trial: the nRF5340 must accept it within %d s or it reverts",
          CONFIG_LPWAN_SHIELD_TRIAL_TIMEOUT_S);
  k_work_init_delayable(&trial_work, trial_fn);
  k_work_schedule(&trial_work, K_SECONDS(5));
}

/* ---- requests ---- */

bool dfu_holds_link(void) {
  switch (core.state) {
  case sensei_DfuState_DFU_STATE_IDLE:
  case sensei_DfuState_DFU_STATE_FAILED:
  case sensei_DfuState_DFU_STATE_DONE:
    return false;
  default:
    return true;
  }
}

void dfu_fill_version(sensei_LpwanVersion *v) {
  v->image_id = running_image_id();
  v->trial = !boot_is_img_confirmed();
  v->dfu_targets = lpwan_dfu_targets(&core);
}

sensei_LpwanResult dfu_handle(const sensei_LpwanRequest *req, sensei_LpwanResponse *rsp) {
  sensei_DfuStatus *st = &rsp->dfu;
  sensei_LpwanResult r;

  rsp->which_payload = sensei_LpwanResponse_dfu_tag;
  switch (req->which_body) {
  case sensei_LpwanRequest_dfu_begin_tag:
    r = lpwan_dfu_begin(&core, &req->dfu_begin, st);
    LOG_INF("dfu begin: target %d, %u B, resume %d -> %d, offset %u", (int)req->dfu_begin.target,
            (unsigned)req->dfu_begin.size, (int)req->dfu_begin.resume, (int)r, (unsigned)st->offset);
    break;
  case sensei_LpwanRequest_dfu_chunk_tag:
    r = lpwan_dfu_chunk(&core, &req->dfu_chunk, st);
    break;
  case sensei_LpwanRequest_dfu_finish_tag:
    r = lpwan_dfu_finish(&core, st);
    LOG_INF("dfu finish -> %d, state %d, fail %d", (int)r, (int)st->state, (int)st->fail);
    break;
  case sensei_LpwanRequest_dfu_apply_tag:
    r = lpwan_dfu_apply(&core, st);
    LOG_INF("dfu apply -> %d, state %d, fail %d", (int)r, (int)st->state, (int)st->fail);
    break;
  case sensei_LpwanRequest_dfu_abort_tag:
    r = lpwan_dfu_abort(&core, st);
    break;
  case sensei_LpwanRequest_dfu_confirm_tag:
    atomic_set(&handshake_ok, 1);
    r = lpwan_dfu_status(&core, st);
    break;
  default:
    r = lpwan_dfu_status(&core, st);
    break;
  }
  return r;
}
