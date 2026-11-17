/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 */

/*
 * App target: the signed image (TF-M and app, one MCUboot image) is written into slot 1 of the flash map
 * (`slot1_partition`, 448 kB at 0x80000). The checkpoints of the transfer are in the first sector of the
 * `storage_partition` (TF-M NS storage is on: the sector is non-secure). The image must carry the kind TLV of this
 * board (checked at `finish`). `dfu_apply` erases the trailer of slot 1 and marks it for one trial boot; `dfu_abort`
 * erases the trailer again. The nRF5340 then cuts the shield power and powers it again, MCUboot swaps and starts the
 * new image, which runs on trial until the nRF5340 accepts the handshake (dfu_confirm, for this image id). Any reset
 * before that brings the old image back.
 *
 * Modem target: nrf_modem_delta_dfu. The delta patch goes to the modem in the order it arrives; the modem erases its
 * DFU area first (minutes), then takes the patch and checks its signature itself. The update runs in the modem at the
 * next start of the modem library. The library is started and restarted on a work queue of its own, so the link
 * thread keeps answering (and feeding its watchdog); the status says ERASING until the modem is ready.
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
#include "ota/sensei_ota_image.h"
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

/* The end of slot 1 holds the MCUboot trailer (magic, flags, swap status: about 1.6 kB for 128 sectors), and the
 * swap with move needs one free sector. The image gets what is left. */
#define TRAILER_SECTORS 2u
#define SPARE_SECTORS 1u

static uint32_t app_capacity(void *ctx, sensei_DfuTarget t) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  return open_areas() == 0 ? (uint32_t)slot1->fa_size - (TRAILER_SECTORS + SPARE_SECTORS) * LPWAN_DFU_SECTOR : 0;
}

/* Erase the trailer of slot 1: stale bytes of an earlier swap make the marking fail, and without it the marking is
 * gone. */
static int erase_trailer(void) {
  for (unsigned i = TRAILER_SECTORS; i > 0; i--) {
    int err = flash_area_erase(slot1, slot1->fa_size - i * LPWAN_DFU_SECTOR, LPWAN_DFU_SECTOR);

    if (err) {
      return err;
    }
    link_feed_watchdog();
  }
  return 0;
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
  int err;

  ARG_UNUSED(ctx);
  ARG_UNUSED(t);
  err = erase_trailer();
  if (err) {
    return err;
  }
  /* One test boot: MCUboot swaps, and goes back at the next reset unless the new image confirms itself. */
  return boot_request_upgrade(BOOT_UPGRADE_TEST);
}

static int app_unmark(void *ctx) {
  ARG_UNUSED(ctx);
  return erase_trailer(); /* no marking, no swap at the next power-up */
}

static int slot1_read(void *ctx, uint32_t off, void *buf, size_t len) {
  ARG_UNUSED(ctx);
  return flash_area_read(slot1, off, buf, len);
}

/* The kind TLV is protected: the signature covers it, so it is the board the image was built and signed for. */
static sensei_DfuFail app_kind(void *ctx) {
  ARG_UNUSED(ctx);
  if (sensei_image_kind_is(slot1_read, NULL, LPWAN_SHIELD_IMAGE_KIND)) {
    return sensei_DfuFail_DFU_FAIL_NONE;
  }
  LOG_ERR("the image is not for this board (no kind TLV '%s')", LPWAN_SHIELD_IMAGE_KIND);
  return sensei_DfuFail_DFU_FAIL_BOARD;
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
    .unmark = app_unmark,
    .check_kind = app_kind,
    .kick = kick,
};

/* ---- modem ---- */

#ifdef CONFIG_LPWAN_SHIELD_MODEM_DFU

/* The modem library is started, and restarted for the update, on a work queue of its own: nrf_modem_lib_init() takes
 * seconds, and minutes while the modem installs an update. The link thread must go on answering. */
static K_THREAD_STACK_DEFINE(modem_wq_stack, CONFIG_LPWAN_SHIELD_MODEM_DFU_STACK);
static struct k_work_q modem_wq;

static bool erase_pending;
static bool write_open;
static struct nrf_modem_delta_dfu_uuid uuid_before;
static bool uuid_known;

enum { OPEN_NONE = 0, OPEN_RUNNING, OPEN_OK, OPEN_FAILED };
static atomic_t open_job;
static int open_err; /* OPEN_FAILED: negative errno, or the modem's positive error code */
static uint32_t open_size;
static bool open_fresh;
static struct k_work open_work;

static atomic_t apply_result; /* 0 running, 1 done, negative: failed (detail in apply_detail) */
static int apply_detail;
static struct k_work apply_work;

/* On modem_wq: start the library, check that the patch fits, and erase the DFU area if needed. */
static int modem_open_sync(uint32_t size, bool fresh) {
  struct nrf_modem_delta_dfu_uuid uuid;
  size_t area = 0, off = 0;
  int err;

  err = lte_modem_init();
  if (err) {
    return err;
  }
  err = nrf_modem_delta_dfu_uuid(&uuid);
  uuid_known = err == 0;
  if (uuid_known) {
    uuid_before = uuid;
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

static void open_fn(struct k_work *w) {
  ARG_UNUSED(w);
  open_err = modem_open_sync(open_size, open_fresh);
  atomic_set(&open_job, open_err ? OPEN_FAILED : OPEN_OK);
}

static int modem_finish(void) {
  int err = 0;

  if (write_open) {
    err = nrf_modem_delta_dfu_write_done();
    write_open = false;
  }
  return err == 0 ? 0 : (err > 0 ? err : -EIO);
}

/* Returns at once: the work runs on modem_wq and modem_busy() says when it is done. */
static int modem_open(uint32_t size, bool fresh) {
  if (atomic_get(&open_job) == OPEN_RUNNING) {
    return 0;
  }
  (void)modem_finish(); /* the write session of an earlier transfer that failed is closed */
  open_size = size;
  open_fresh = fresh;
  erase_pending = false;
  atomic_set(&open_job, OPEN_RUNNING);
  k_work_submit_to_queue(&modem_wq, &open_work);
  return 0;
}

/* > 0 while the modem library starts or the modem erases its DFU area (it can take minutes), 0 ready, < 0 an error. */
static int modem_busy(void) {
  size_t off;
  int err;

  switch (atomic_get(&open_job)) {
  case OPEN_RUNNING:
    return 1;
  case OPEN_FAILED:
    return open_err == 0 ? -EIO : (open_err > 0 ? -open_err : open_err);
  default:
    break;
  }
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

static int dfu_result;
NRF_MODEM_LIB_ON_DFU_RES(sensei_dfu, on_dfu_res, NULL);
static void on_dfu_res(int dfu_res, void *ctx) {
  ARG_UNUSED(ctx);
  dfu_result = dfu_res;
}

/* On modem_wq: the update runs when the modem library starts, so shut it down and start it again. Success is the
 * result the library reports (NRF_MODEM_DFU_RESULT_OK), or, when it reports nothing, a modem UUID that changed. A
 * result of VOLTAGE_LOW is a failure here; the modem tries again at its next start (the next LTE connect). */
static void apply_fn(struct k_work *w) {
  struct nrf_modem_delta_dfu_uuid now;
  bool ok = false;
  int err;

  ARG_UNUSED(w);
  dfu_result = 0;
  err = nrf_modem_lib_shutdown();
  if (err == 0) {
    err = nrf_modem_lib_init();
  }
  lte_modem_restarted(err == 0);
  if (err == 0) {
    if (dfu_result == (int)NRF_MODEM_DFU_RESULT_OK) {
      ok = true;
    } else if (dfu_result == 0) {
      ok = uuid_known && nrf_modem_delta_dfu_uuid(&now) == 0 && memcmp(&now, &uuid_before, sizeof(now)) != 0;
    }
  }
  if (ok) {
    apply_detail = 0;
    atomic_set(&apply_result, 1);
    LOG_INF("modem firmware updated");
  } else {
    apply_detail = dfu_result != 0 ? dfu_result : (err != 0 ? err : -ENOMSG);
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
  k_work_submit_to_queue(&modem_wq, &apply_work);
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

static int slot0_read(void *ctx, uint32_t off, void *buf, size_t len) {
  return flash_area_read((const struct flash_area *)ctx, off, buf, len);
}

/* First 4 bytes of the SHA-256 TLV of the running image (the id the nRF5340 and the host see in LpwanVersion). */
static uint32_t running_image_id(void) {
  static uint32_t id;
  static bool known;
  const struct flash_area *fa;

  if (!known) {
    if (flash_area_open(SLOT0_ID, &fa) != 0) {
      return 0;
    }
    id = sensei_image_id(slot0_read, (void *)fa);
    flash_area_close(fa);
    known = true;
  }
  return id;
}

static atomic_t handshake_ok; /* the nRF5340 accepted this image (dfu_confirm for this image id) */
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
  k_work_queue_init(&modem_wq);
  k_work_queue_start(&modem_wq, modem_wq_stack, K_THREAD_STACK_SIZEOF(modem_wq_stack),
                     K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_LINK_PRIO + 2), NULL);
  k_thread_name_set(&modem_wq.thread, "modem_dfu");
  k_work_init(&open_work, open_fn);
  k_work_init(&apply_work, apply_fn);
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

/* The radios wait while a transfer is open and its sender is around, and while the modem works. A sender that has
 * been quiet for CONFIG_LPWAN_SHIELD_DFU_HOLD_S lets the radios go (the transfer stays and can be resumed). */
bool dfu_holds_link(void) {
  return lpwan_dfu_holds_link(&core, k_uptime_get_32(), CONFIG_LPWAN_SHIELD_DFU_HOLD_S * 1000U);
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
  lpwan_dfu_touch(&core, k_uptime_get_32());
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
    LOG_INF("dfu abort -> %d, state %d", (int)r, (int)st->state);
    break;
  case sensei_LpwanRequest_dfu_confirm_tag:
    r = lpwan_dfu_status(&core, st);
    if (lpwan_dfu_confirm_allowed(req->dfu_confirm.image_id, running_image_id())) {
      atomic_set(&handshake_ok, 1); /* sent again until the image has confirmed: harmless to repeat */
    } else {
      LOG_WRN("dfu_confirm for image %08x, running %08x: ignored", (unsigned)req->dfu_confirm.image_id,
              (unsigned)running_image_id());
      st->fail = sensei_DfuFail_DFU_FAIL_STATE;
      r = sensei_LpwanResult_LPWAN_INVALID;
    }
    break;
  default:
    r = lpwan_dfu_status(&core, st);
    break;
  }
  return r;
}
