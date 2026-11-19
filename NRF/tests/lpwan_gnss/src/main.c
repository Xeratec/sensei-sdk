/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The GNSS fix of the LPWAN client (lpwan_client.c) against a scripted shield. The shield is a thread of this test
 * that listens on the port of CONFIG_SENSEI_LPWAN_SIM_TARGET, speaks the link of the simulated shield (lpwan_sim.h),
 * decides every request with the same function as the shield app (lpwan_admit_request) and keeps a log of the requests.
 * The test ends a fix by hand (fake_end_fix) or lets it run for ever, and cuts or keeps the power from the client side.
 *
 * What is checked: an LTE send during a fix goes out; an LTE connect waits for the fix (no configure or connect
 * request reaches the shield) and the app is called (on_up) after it; a disconnect asked during the fix runs when it
 * has ended; a result that never comes ends with -ETIMEDOUT and a cancel request; power off during a fix ends it with
 * -ENOTCONN and the next fix works; a result that arrives before the answer to the request is taken; an update of the
 * shield is refused (-EBUSY); the time of the fix is moved to now, counted from the EVENT edge of the result and not
 * from the moment the client thread read it (a slow answer to another request holds the thread), also with an earlier
 * event queued before it; a release that the shield refuses (BUSY: it still runs the fix after the client gave it up)
 * is tried again, five times, and again after a give-up; a connect that comes meanwhile cancels the retry. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "lpwan_priv.h"
#include "sensei_lpwan/link_slave.h"
#include "sensei_lpwan/lpwan.h"
#include "sensei_lpwan/lpwan_gnss.h"
#include "sensei_lpwan/lpwan_sim.h"

#define PORT 17101
#define FIX_UTC 1780272000u /* the time of the fix that the fake shield reports, Unix s */
#define FIX_AGE_MS 1250u    /* its age when the shield sends the result */
#define LOG_N 96

/* ---- the scripted shield ---- */

struct entry {
  uint16_t tag;           /* the request tag, or TAG_FIX_END */
  sensei_LpwanResult res; /* the answer */
  bool fix_running;       /* a fix ran when the request came */
};
#define TAG_FIX_END 0xFFFFu

static struct {
  int lfd, cfd;
  struct link_slave slv;
  lpwan_sm_t sm;
  const uint8_t *armed_tx;
  uint8_t *armed_rx;
  bool armed, busy, event;
  struct lpwan_sim_rx rx;
  bool fix_running;
  uint32_t fix_seq;
  /* script */
  bool result_in_request; /* the result of a fix is queued before the answer to the request */
  int send_delay_ms;      /* the answer to a send comes this late (the client thread waits for it) */
  uint32_t age_ms;        /* age of the fix in the result, ms */
  bool ignore_cancel;     /* the shield takes a gnss_cancel and goes on with the fix a while */
  /* observations */
  struct entry log[LOG_N];
  int n_log;
  int cuts;
} fk;

static K_MUTEX_DEFINE(st_lock); /* fk.sm, fk.fix_running, the log; outermost */
static K_MUTEX_DEFINE(q_lock);  /* the event queue of the slave core */
static K_MUTEX_DEFINE(tx_lock); /* writes to the client, the pin levels */
static struct k_thread fake_thread;
K_THREAD_STACK_DEFINE(fake_stack, 4096);

static void write_frame(uint8_t type, const uint8_t *p, size_t n) {
  static uint8_t out[LPWAN_SIM_HEADER + LPWAN_XFER_LEN];
  size_t len = lpwan_sim_pack(type, p, n, out);

  if (fk.cfd >= 0 && len > 0) {
    (void)lpwan_sim_write(fk.cfd, out, len);
  }
}

static void send_pins(void) {
  const uint8_t pins[2] = {fk.busy, fk.event};

  write_frame(LPWAN_SIM_PINS, pins, sizeof(pins));
}

static int op_arm(void *ctx, const uint8_t *tx, uint8_t *rx_buf, size_t len) {
  ARG_UNUSED(ctx);
  if (len != LPWAN_XFER_LEN) {
    return -EINVAL;
  }
  k_mutex_lock(&tx_lock, K_FOREVER);
  fk.armed_tx = tx;
  fk.armed_rx = rx_buf;
  fk.armed = true;
  k_mutex_unlock(&tx_lock);
  return 0;
}

static void op_busy(void *ctx, bool b) {
  ARG_UNUSED(ctx);
  k_mutex_lock(&tx_lock, K_FOREVER);
  if (b != fk.busy) {
    fk.busy = b;
    send_pins();
  }
  k_mutex_unlock(&tx_lock);
}

static void op_event(void *ctx, bool pending) {
  ARG_UNUSED(ctx);
  k_mutex_lock(&tx_lock, K_FOREVER);
  if (pending != fk.event) {
    fk.event = pending;
    send_pins();
  }
  k_mutex_unlock(&tx_lock);
}

static void op_lock(void *ctx) {
  ARG_UNUSED(ctx);
  k_mutex_lock(&q_lock, K_FOREVER);
}

static void op_unlock(void *ctx) {
  ARG_UNUSED(ctx);
  k_mutex_unlock(&q_lock);
}

static const struct link_slave_ops fk_ops = {
    .arm = op_arm, .set_busy = op_busy, .set_event = op_event, .lock = op_lock, .unlock = op_unlock};

static void log_add(uint16_t tag, sensei_LpwanResult res) {
  if (fk.n_log < LOG_N) {
    fk.log[fk.n_log++] = (struct entry){.tag = tag, .res = res, .fix_running = fk.fix_running};
  }
}

/* With st_lock: the shield's state machine takes the event as the shield app does (svc_emit), then the master gets it.
 */
static void push(const sensei_LpwanEvent *e) {
  (void)lpwan_sm_event(&fk.sm, e);
  (void)link_slave_push_event(&fk.slv, e);
}

static void end_fix_locked(sensei_GnssFixStatus status) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  if (!fk.fix_running) {
    return;
  }
  log_add(TAG_FIX_END, sensei_LpwanResult_LPWAN_OK);
  fk.fix_running = false; /* before the event, as gnss.c does */
  e.which_body = sensei_LpwanEvent_gnss_result_tag;
  e.gnss_result.seq = fk.fix_seq;
  e.gnss_result.status = status;
  if (status == sensei_GnssFixStatus_GNSS_FIX_OK) {
    e.gnss_result.lat_e7 = 473764000;
    e.gnss_result.lon_e7 = 85476000;
    e.gnss_result.alt_m = 410;
    e.gnss_result.acc_m = 5;
    e.gnss_result.sats = 8;
    e.gnss_result.unix_time = FIX_UTC;
    e.gnss_result.age_ms = fk.age_ms;
    e.gnss_result.ttff_ms = 31000;
  }
  push(&e);
}

static void fake_end_fix(sensei_GnssFixStatus status) {
  k_mutex_lock(&st_lock, K_FOREVER);
  end_fix_locked(status);
  k_mutex_unlock(&st_lock);
}

static void on_request(struct link_slave *s, const sensei_LpwanRequest *req, void *u) {
  sensei_LpwanResponse r = sensei_LpwanResponse_init_zero;
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;
  bool logged = false;

  ARG_UNUSED(u);
  r.seq = req->seq;
  if (req->which_body == sensei_LpwanRequest_send_tag && fk.send_delay_ms > 0) {
    k_msleep(fk.send_delay_ms); /* before the lock: the test may end the fix meanwhile */
  }
  k_mutex_lock(&st_lock, K_FOREVER);
  r.result = lpwan_admit_request(&fk.sm, req, false, fk.fix_running); /* the function of service.c */
  if (r.result == sensei_LpwanResult_LPWAN_OK) {
    switch (req->which_body) {
    case sensei_LpwanRequest_get_version_tag:
      r.which_payload = sensei_LpwanResponse_version_tag;
      r.version.has_firmware = true;
      r.version.protocol = LPWAN_PROTOCOL_VERSION;
      r.version.gnss = true;
      r.version.dfu_targets = 3;
      break;
    case sensei_LpwanRequest_get_status_tag:
      r.which_payload = sensei_LpwanResponse_status_tag;
      break;
    case sensei_LpwanRequest_lte_connect_tag:
      e.which_body = sensei_LpwanEvent_connected_tag;
      e.connected.radio = sensei_Radio_RADIO_LTEM;
      push(&e);
      break;
    case sensei_LpwanRequest_lte_disconnect_tag:
      e.which_body = sensei_LpwanEvent_disconnected_tag;
      e.disconnected.radio = sensei_Radio_RADIO_LTEM;
      push(&e);
      break;
    case sensei_LpwanRequest_send_tag:
      e.which_body = sensei_LpwanEvent_tx_done_tag;
      e.tx_done.seq = req->seq;
      e.tx_done.ok = true;
      push(&e);
      break;
    case sensei_LpwanRequest_gnss_fix_tag:
      fk.fix_running = true;
      fk.fix_seq = req->seq;
      log_add(req->which_body, r.result);
      logged = true;
      if (fk.result_in_request) {
        end_fix_locked(sensei_GnssFixStatus_GNSS_FIX_OK); /* queued before the answer is read */
      }
      break;
    case sensei_LpwanRequest_gnss_cancel_tag:
      if (!fk.ignore_cancel) {
        end_fix_locked(sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
      }
      break;
    default:
      break;
    }
  }
  if (!logged) {
    log_add(req->which_body, r.result);
  }
  k_mutex_unlock(&st_lock);
  link_slave_respond(s, &r);
}

static void on_xfer(const uint8_t *frame, size_t len) {
  static uint8_t rsp[LPWAN_XFER_LEN];
  bool was_armed;

  k_mutex_lock(&tx_lock, K_FOREVER);
  was_armed = fk.armed && len == LPWAN_XFER_LEN;
  if (was_armed) {
    memcpy(fk.armed_rx, frame, LPWAN_XFER_LEN);
    memcpy(rsp, fk.armed_tx, LPWAN_XFER_LEN);
    fk.armed = false;
    fk.busy = true;
    send_pins();
  } else {
    memset(rsp, 0, sizeof(rsp));
  }
  write_frame(LPWAN_SIM_XFER_RSP, rsp, sizeof(rsp));
  k_mutex_unlock(&tx_lock);
  if (was_armed) {
    (void)link_slave_done(&fk.slv, 0);
  }
}

/* The client cut the power: the shield has lost everything. */
static void fake_power_loss(void) {
  lpwan_sim_close(fk.cfd);
  fk.cfd = -1;
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.cuts++;
  fk.fix_running = false;
  fk.armed = false;
  fk.busy = true;
  fk.event = false;
  memset(&fk.slv, 0, sizeof(fk.slv));
  link_slave_init(&fk.slv, &fk_ops, NULL, on_request, NULL);
  lpwan_sm_init(&fk.sm);
  k_mutex_unlock(&st_lock);
  fk.lfd = lpwan_sim_listen("127.0.0.1", PORT);
}

static void fake_main(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  fk.cfd = -1;
  fk.busy = true;
  link_slave_init(&fk.slv, &fk_ops, NULL, on_request, NULL);
  lpwan_sm_init(&fk.sm);
  fk.lfd = lpwan_sim_listen("127.0.0.1", PORT);
  for (;;) {
    uint8_t type;
    const uint8_t *p;
    size_t len;
    int n, r;

    if (fk.cfd < 0) {
      int fd = fk.lfd >= 0 ? lpwan_sim_accept(fk.lfd) : -2;

      if (fd >= 0) {
        fk.cfd = fd;
        lpwan_sim_close(fk.lfd);
        fk.lfd = -1;
        fk.rx.n = 0;
        k_mutex_lock(&tx_lock, K_FOREVER);
        fk.busy = true;
        fk.event = false;
        send_pins();
        k_mutex_unlock(&tx_lock);
        while (link_slave_start(&fk.slv) != 0) {
          k_msleep(10);
        }
        continue;
      }
      if (fk.lfd < 0) {
        fk.lfd = lpwan_sim_listen("127.0.0.1", PORT);
      }
      k_msleep(1);
      continue;
    }
    n = lpwan_sim_read(fk.cfd, fk.rx.buf + fk.rx.n, lpwan_sim_rx_room(&fk.rx));
    if (n == -2) {
      k_msleep(1);
      continue;
    }
    if (n <= 0) {
      fake_power_loss();
      continue;
    }
    fk.rx.n += (size_t)n;
    while ((r = lpwan_sim_rx_next(&fk.rx, &type, &p, &len)) == 1) {
      if (type == LPWAN_SIM_XFER) {
        on_xfer(p, len);
      }
      lpwan_sim_rx_pop(&fk.rx);
    }
    if (r < 0) {
      fake_power_loss();
    }
  }
}

/* ---- observations ---- */

static int log_count(uint16_t tag) {
  int n = 0;

  k_mutex_lock(&st_lock, K_FOREVER);
  for (int i = 0; i < fk.n_log; i++) {
    n += fk.log[i].tag == tag;
  }
  k_mutex_unlock(&st_lock);
  return n;
}

/* Index of the first entry with this tag after index `from`, -1 if none. */
static int log_find(uint16_t tag, int from) {
  int r = -1;

  k_mutex_lock(&st_lock, K_FOREVER);
  for (int i = from < 0 ? 0 : from; i < fk.n_log; i++) {
    if (fk.log[i].tag == tag) {
      r = i;
      break;
    }
  }
  k_mutex_unlock(&st_lock);
  return r;
}

static bool fake_fix_running(void) {
  bool r;

  k_mutex_lock(&st_lock, K_FOREVER);
  r = fk.fix_running;
  k_mutex_unlock(&st_lock);
  return r;
}

static lpwan_sm_t fake_sm(void) {
  lpwan_sm_t s;

  k_mutex_lock(&st_lock, K_FOREVER);
  s = fk.sm;
  k_mutex_unlock(&st_lock);
  return s;
}

/* ---- the client side ---- */

static int up_lte, up_ble;
static int sent_calls, sent_err;
static int fix_calls, fix_err;
static sensei_GnssFixResult fix_res;
static int64_t fix_call_ms;
static int dfu_calls, dfu_err;

static void on_up(sensei_Radio r) {
  if (r == sensei_Radio_RADIO_LTEM) {
    up_lte++;
  } else if (r == sensei_Radio_RADIO_BLE) {
    up_ble++;
  }
}

static void send_done(int err, void *user) {
  ARG_UNUSED(user);
  sent_err = err;
  sent_calls++;
}

static void fix_done(int err, const sensei_GnssFixResult *res, void *user) {
  ARG_UNUSED(user);
  fix_err = err;
  fix_res = *res;
  fix_call_ms = k_uptime_get();
  fix_calls++;
}

static void dfu_done(int err, const struct sensei_lpwan_dfu_result *res, void *user) {
  ARG_UNUSED(res);
  ARG_UNUSED(user);
  dfu_err = err;
  dfu_calls++;
}

/* Poll until *v reaches n, at most `ms` of (simulated) time. */
static bool wait_int(const int *v, int n, int ms) {
  for (int t = 0; t < ms && *v < n; t += 10) {
    k_msleep(10);
  }
  return *v >= n;
}

static bool wait_fix_running(bool want, int ms) {
  for (int t = 0; t < ms && fake_fix_running() != want; t += 10) {
    k_msleep(10);
  }
  return fake_fix_running() == want;
}

static bool wait_log(uint16_t tag, int n, int ms) {
  for (int t = 0; t < ms && log_count(tag) < n; t += 10) {
    k_msleep(10);
  }
  return log_count(tag) >= n;
}

static void *suite_setup(void) {
  struct sensei_lpwan_hooks hooks = {.on_up = on_up};
  struct sensei_lpwan_config cfg = {.lte_port = 1};

  strcpy(cfg.lte_server, "collector.test");
  k_thread_create(&fake_thread, fake_stack, K_THREAD_STACK_SIZEOF(fake_stack), fake_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
  k_thread_name_set(&fake_thread, "fake_shield");
  zassert_ok(sensei_lpwan_init(&hooks, &cfg));
  return NULL;
}

/* Every test starts with the shield off: the client cuts the power, the fake sees it and starts over. */
static void before(void *f) {
  ARG_UNUSED(f);
  (void)sensei_lpwan_cmd_power(false); /* a no-op if it is off */
  k_msleep(600);
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.n_log = 0;
  fk.result_in_request = false;
  fk.send_delay_ms = 0;
  fk.age_ms = FIX_AGE_MS;
  fk.ignore_cancel = false;
  k_mutex_unlock(&st_lock);
  up_lte = up_ble = 0;
  sent_calls = fix_calls = dfu_calls = 0;
  sent_err = fix_err = dfu_err = 0;
  memset(&fix_res, 0, sizeof(fix_res));
}

/* The app's way: ask whether the radio is ready, which wakes the connection; on_up says when it is. */
static void connect_lte(void) {
  zassert_false(sensei_lpwan_radio_ready(sensei_Radio_RADIO_LTEM));
  zassert_true(wait_int(&up_lte, 1, 20000), "LTE did not connect");
  zassert_true(sensei_lpwan_radio_ready(sensei_Radio_RADIO_LTEM));
}

static void start_fix(uint32_t timeout_s) {
  zassert_ok(sensei_lpwan_gnss_fix(timeout_s, fix_done, NULL));
  zassert_true(wait_fix_running(true, 20000), "the shield did not get the fix request");
}

ZTEST(lpwan_gnss, test_lte_send_goes_out_during_a_fix) {
  static const uint8_t data[4] = {1, 2, 3, 4};

  connect_lte();
  start_fix(60);
  zassert_true(sensei_lpwan_gnss_busy());
  /* LTE is connected: the client does not hold the radio back */
  zassert_true(sensei_lpwan_radio_ready(sensei_Radio_RADIO_LTEM));
  zassert_ok(sensei_lpwan_radio_send(sensei_Radio_RADIO_LTEM, data, sizeof(data), send_done, NULL));
  zassert_true(wait_int(&sent_calls, 1, 20000), "the send did not end");
  zassert_equal(sent_err, 0);
  int i = log_find(sensei_LpwanRequest_send_tag, 0);

  zassert_true(i >= 0);
  zassert_true(fk.log[i].fix_running, "the send came while the fix ran");
  zassert_equal(fk.log[i].res, sensei_LpwanResult_LPWAN_OK, "the shield took it");
  zassert_equal(fix_calls, 0, "the fix runs on");

  /* the result: the time of the fix is moved to now (age 1.25 s at the shield, and the delay at the client) */
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_err, 0);
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_equal(fix_res.lat_e7, 473764000);
  zassert_true(fix_res.unix_time >= FIX_UTC + 1 && fix_res.unix_time <= FIX_UTC + 3, "time %u",
               (unsigned)fix_res.unix_time);
  zassert_false(sensei_lpwan_gnss_busy());
}

ZTEST(lpwan_gnss, test_result_time_counts_from_the_event_edge) {
  static const uint8_t data[4] = {1, 2, 3, 4};

  /* The client thread waits for the (slow) answer to a send while the result arrives: it reads the result 0.8 s after
   * its EVENT edge. The result is 1.1 s old when the shield sends it, so it is 1.9 s old when the callback runs: +2 s
   * (the time is rounded to seconds). Counted from the moment of the read it would be 1.1 s old: +1 s, the clock
   * would be set back by the 0.8 s. */
  connect_lte();
  start_fix(60);
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.age_ms = 1100;
  fk.send_delay_ms = 800;
  k_mutex_unlock(&st_lock);
  zassert_ok(sensei_lpwan_radio_send(sensei_Radio_RADIO_LTEM, data, sizeof(data), send_done, NULL));
  k_msleep(100); /* the request is at the shield, its answer is held */
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_true(wait_int(&sent_calls, 1, 20000));
  zassert_equal(fix_err, 0);
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(fix_res.unix_time >= FIX_UTC + 2 && fix_res.unix_time <= FIX_UTC + 3, "time +%d s",
               (int)(fix_res.unix_time - FIX_UTC));
}

ZTEST(lpwan_gnss, test_release_is_tried_again_after_busy) {
  /* The client gave the fix up after its deadline and asked the shield to cancel, but the shield still runs it: the
   * release that waited for the fix is refused (BUSY). The client asks again a few seconds later; the shield has ended
   * the fix by then and takes it. */
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.ignore_cancel = true;
  k_mutex_unlock(&st_lock);
  connect_lte();
  start_fix(1);
  zassert_ok(sensei_lpwan_cmd_disconnect()); /* during the fix: it waits */
  zassert_true(wait_int(&fix_calls, 1, 60000));
  zassert_equal(fix_err, -ETIMEDOUT);
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 1, 5000), "the release was tried");
  int first = log_find(sensei_LpwanRequest_lte_disconnect_tag, 0);

  zassert_equal(fk.log[first].res, sensei_LpwanResult_LPWAN_BUSY, "the shield still runs the fix");
  zassert_equal(fake_sm().tech, LPWAN_TECH_LTE);
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_CANCELLED); /* the shield is done */
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 2, 10000), "no second try");
  k_msleep(300);
  int ok = log_find(sensei_LpwanRequest_lte_disconnect_tag, first + 1);

  zassert_true(ok > first);
  zassert_equal(fk.log[ok].res, sensei_LpwanResult_LPWAN_OK);
  zassert_equal(fake_sm().tech, LPWAN_TECH_NONE, "released");
}

ZTEST(lpwan_gnss, test_result_time_with_several_events_queued) {
  static const uint8_t data[4] = {1, 2, 3, 4};
  sensei_LpwanEvent stray = sensei_LpwanEvent_init_zero;

  /* EVENT stays high while events are queued: the interrupt sees the edge of the first one only. An earlier event is
   * queued at 0.1 s and the result at 0.7 s; the client thread is held by a slow answer until 1.5 s and reads both
   * then. The result is not older than 0.8 s, and with age 0 it must not be moved on by more than that: +1 s at most.
   * Counted from the edge of the first event (1.4 s) it would be +2 s, the clock ahead. */
  stray.which_body = sensei_LpwanEvent_error_tag;
  stray.error.code = sensei_LpwanResult_LPWAN_BUSY;
  connect_lte();
  start_fix(60);
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.age_ms = 0;
  fk.send_delay_ms = 1500;
  k_mutex_unlock(&st_lock);
  zassert_ok(sensei_lpwan_radio_send(sensei_Radio_RADIO_LTEM, data, sizeof(data), send_done, NULL));
  k_msleep(100); /* the request is at the shield, its answer is held */
  k_mutex_lock(&st_lock, K_FOREVER);
  push(&stray);
  k_mutex_unlock(&st_lock);
  k_msleep(600);
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_true(wait_int(&sent_calls, 1, 20000));
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(fix_res.unix_time <= FIX_UTC + 1, "time +%d s: counted from the first event's edge",
               (int)(fix_res.unix_time - FIX_UTC));
}

ZTEST(lpwan_gnss, test_release_gives_up_and_a_later_release_is_tried_again) {
  /* The shield keeps refusing the release (BUSY). The client tries RELEASE_TRIES_MAX more times, then gives up and
   * resets the count: a release that comes later gets its tries again (it did not: the count stayed at 6). */
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.ignore_cancel = true;
  k_mutex_unlock(&st_lock);
  connect_lte();
  start_fix(1);
  zassert_ok(sensei_lpwan_cmd_disconnect());
  zassert_true(wait_int(&fix_calls, 1, 60000));
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 6, 60000), "the first release and five tries");
  k_msleep(10000);
  zassert_equal(log_count(sensei_LpwanRequest_lte_disconnect_tag), 6, "no seventh try: given up");
  zassert_ok(sensei_lpwan_cmd_disconnect()); /* a later release */
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 8, 20000), "no new tries after the give-up");
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_CANCELLED); /* the shield is done: the next try goes through */
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 9, 10000));
  k_msleep(300);
  zassert_equal(fake_sm().tech, LPWAN_TECH_NONE, "released");
}

ZTEST(lpwan_gnss, test_a_connect_cancels_the_release_that_waits_for_its_retry) {
  /* The release was refused (BUSY) and waits for its retry. LTE is wanted again meanwhile: the retry does nothing. */
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.ignore_cancel = true;
  k_mutex_unlock(&st_lock);
  connect_lte();
  start_fix(1);
  zassert_ok(sensei_lpwan_cmd_disconnect());
  zassert_true(wait_int(&fix_calls, 1, 60000));
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 1, 5000), "the release was tried");
  zassert_ok(sensei_lpwan_cmd_connect(sensei_Radio_RADIO_LTEM)); /* LTE is wanted again */
  k_msleep(300);
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
  k_msleep(8000); /* the retry timer has run out */
  zassert_equal(log_count(sensei_LpwanRequest_lte_disconnect_tag), 1, "the retry released a connection that is wanted");
  zassert_equal(fake_sm().tech, LPWAN_TECH_LTE);
}

ZTEST(lpwan_gnss, test_lte_connect_waits_for_the_fix) {
  start_fix(60);
  /* LTE is not connected: the app asks, the client holds the connect back (no configure, no connect at the shield) */
  zassert_false(sensei_lpwan_radio_ready(sensei_Radio_RADIO_LTEM));
  k_msleep(1500);
  zassert_equal(log_count(sensei_LpwanRequest_lte_configure_tag), 0);
  zassert_equal(log_count(sensei_LpwanRequest_lte_connect_tag), 0);
  zassert_equal(up_lte, 0);
  /* the fix ends: on_up(RADIO_BLE) tells the app, which asks again and gets the connection */
  int up0 = up_ble;

  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_TIMEOUT);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_TIMEOUT);
  zassert_true(wait_int(&up_ble, up0 + 1, 5000), "no on_up after the fix");
  connect_lte();
  int end = log_find(TAG_FIX_END, 0);
  int conn = log_find(sensei_LpwanRequest_lte_connect_tag, 0);

  zassert_true(end >= 0 && conn > end, "the connect came after the fix");
}

ZTEST(lpwan_gnss, test_disconnect_is_deferred_until_the_fix_ends) {
  connect_lte();
  start_fix(60);
  zassert_ok(sensei_lpwan_cmd_disconnect());
  k_msleep(2000);
  zassert_equal(log_count(sensei_LpwanRequest_lte_disconnect_tag), 0, "no release during the fix");
  zassert_equal(fake_sm().tech, LPWAN_TECH_LTE);
  zassert_equal(fix_calls, 0);

  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_true(wait_log(sensei_LpwanRequest_lte_disconnect_tag, 1, 20000), "the release was lost");
  int end = log_find(TAG_FIX_END, 0);
  int rel = log_find(sensei_LpwanRequest_lte_disconnect_tag, 0);

  zassert_true(rel > end, "it ran after the fix");
  zassert_equal(fk.log[rel].res, sensei_LpwanResult_LPWAN_OK, "and the shield took it");
  k_msleep(300);
  zassert_equal(fake_sm().tech, LPWAN_TECH_NONE);
}

ZTEST(lpwan_gnss, test_no_result_ends_with_a_timeout_and_a_cancel) {
  int64_t t0 = k_uptime_get();

  start_fix(1); /* the shield never answers with a result: the deadline is 1 s + the margin */
  zassert_true(wait_int(&fix_calls, 1, 60000));
  zassert_true(fix_call_ms - t0 >= (1 + LPWAN_GNSS_MARGIN_S) * 1000 - 100, "ended before the margin (%d ms)",
               (int)(fix_call_ms - t0));
  zassert_equal(fix_err, -ETIMEDOUT);
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_FAILED);
  zassert_true(wait_log(sensei_LpwanRequest_gnss_cancel_tag, 1, 5000), "the shield may still run it: cancel");
  zassert_false(sensei_lpwan_gnss_busy());
  /* the next fix is not refused */
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
  fix_calls = 0;
  start_fix(30);
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_err, 0);
}

ZTEST(lpwan_gnss, test_cancel_of_the_node_ends_the_fix) {
  start_fix(60);
  zassert_ok(sensei_lpwan_gnss_cancel());
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_err, 0);
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
  zassert_equal(log_count(sensei_LpwanRequest_gnss_cancel_tag), 1);
}

ZTEST(lpwan_gnss, test_power_off_during_a_fix) {
  int cuts;

  start_fix(60);
  zassert_ok(sensei_lpwan_cmd_power(false));
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_err, -ENOTCONN, "the shield is gone");
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
  zassert_false(sensei_lpwan_gnss_busy());
  k_msleep(500);
  k_mutex_lock(&st_lock, K_FOREVER);
  cuts = fk.cuts;
  k_mutex_unlock(&st_lock);
  zassert_true(cuts >= 1, "the power was cut");
  /* the shield comes up again for the next fix */
  fix_calls = 0;
  start_fix(30);
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_OK);
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_err, 0);
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_OK);
}

ZTEST(lpwan_gnss, test_result_that_comes_during_the_request) {
  /* The shield sends the result before the answer to the request is read: it counts, once, and the client does not
   * wait for a second result (no deadline, no cancel). */
  k_mutex_lock(&st_lock, K_FOREVER);
  fk.result_in_request = true;
  k_mutex_unlock(&st_lock);
  zassert_ok(sensei_lpwan_gnss_fix(5, fix_done, NULL));
  zassert_true(wait_int(&fix_calls, 1, 20000));
  zassert_equal(fix_err, 0);
  zassert_equal(fix_res.status, sensei_GnssFixStatus_GNSS_FIX_OK);
  k_msleep((5 + LPWAN_GNSS_MARGIN_S + 5) * 1000);
  zassert_equal(fix_calls, 1, "one callback");
  zassert_equal(log_count(sensei_LpwanRequest_gnss_cancel_tag), 0, "no cancel");
  zassert_false(sensei_lpwan_gnss_busy());
}

ZTEST(lpwan_gnss, test_update_of_the_shield_is_refused_during_a_fix) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  start_fix(60);
  rq.which_body = sensei_LpwanRequest_dfu_begin_tag;
  zassert_ok(sensei_lpwan_dfu_submit(&rq, dfu_done, NULL));
  zassert_true(wait_int(&dfu_calls, 1, 20000));
  zassert_equal(dfu_err, -EBUSY);
  zassert_equal(log_count(sensei_LpwanRequest_dfu_begin_tag), 0, "the shield never saw it");
  zassert_false(sensei_lpwan_dfu_active());
  fake_end_fix(sensei_GnssFixStatus_GNSS_FIX_TIMEOUT);
  zassert_true(wait_int(&fix_calls, 1, 20000));
}

ZTEST_SUITE(lpwan_gnss, NULL, suite_setup, before, NULL, NULL);
