/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The SPI link. Every transaction is one chip select: GAP9 clocks a frame out (push) or clocks
 * the frame the nRF5340 has armed in (poll). The trigger line tells when the nRF5340 is armed. */

#include <string.h>

#include "pmsis.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include "sensei_gap9/gap9_link.h"
#include "sensei_gap9/gap9_side.h"
#include "sensei_link/pb.h"
#include "sensei_link/spi.h"

#if G9L_LOG_LEVEL >= 0
#define LOG_ERR(...) printf("[GAP9-ERR] " __VA_ARGS__)
#else
#define LOG_ERR(...)
#endif
#if G9L_LOG_LEVEL >= 1
#define LOG_WRN(...) printf("[GAP9-WRN] " __VA_ARGS__)
#else
#define LOG_WRN(...)
#endif

static struct g9l_cfg cfg;
static struct pi_device spi_dev;
static struct pi_spi_conf spi_conf;
static pi_evt_t trigger_task;
static SemaphoreHandle_t trigger_sem;

/* GAP9 to nRF5340 frames are at most G9L_RX_CAP bytes; jobs up to G9L_TX_CAP. */
static PI_L2 uint8_t tx_frame[G9L_RX_CAP];
static PI_L2 uint8_t rx_frame[G9L_TX_CAP];
static PI_L2 uint8_t poll_hdr[G9L_HDR];

static void cs(int level) { pi_gpio_pin_write(cfg.pad_cs, level); }

static void trigger_cb(void *arg) {
  BaseType_t woken = pdFALSE;

  (void)arg;
  xSemaphoreGiveFromISR(trigger_sem, &woken);
}

int g9l_init(const struct g9l_cfg *c) {
  cfg = *c;
  pi_pad_function_set(cfg.pad_sck, PI_PAD_FUNC0);
  pi_pad_function_set(cfg.pad_mosi, PI_PAD_FUNC0);
  pi_pad_function_set(cfg.pad_miso, PI_PAD_FUNC0);
  pi_pad_function_set(cfg.pad_cs, PI_PAD_FUNC1);
  pi_gpio_pin_configure(cfg.pad_cs, PI_GPIO_OUTPUT | PI_GPIO_DRIVE_STRENGTH_HIGH);
  cs(1);

  pi_spi_conf_init(&spi_conf);
  spi_conf.wordsize = PI_SPI_WORDSIZE_8;
  spi_conf.big_endian = 1;
  spi_conf.max_baudrate = cfg.spi_hz != 0 ? cfg.spi_hz : G9L_SPI_HZ_DEFAULT;
  spi_conf.polarity = 1;
  spi_conf.phase = 1;
  spi_conf.itf = cfg.spi_itf;
  spi_conf.cs = PI_SPI_CS_NONE; /* manual CS */
  pi_open_from_conf(&spi_dev, &spi_conf);
  if (pi_spi_open(&spi_dev)) {
    LOG_ERR("SPI open failed\n");
    return -1;
  }

  pi_pad_function_set(cfg.pad_trigger, PI_PAD_FUNC1);
  pi_gpio_pin_configure(cfg.pad_trigger, PI_GPIO_INPUT | PI_GPIO_PULL_DISABLE);
  trigger_sem = xSemaphoreCreateBinary();
  pi_evt_callback_no_irq_init(&trigger_task, trigger_cb, NULL);
  if (trigger_sem == NULL || pi_gpio_pin_task_add(cfg.pad_trigger, &trigger_task, PI_GPIO_NOTIF_RISE)) {
    LOG_ERR("trigger setup failed\n");
    return -1;
  }
  return 0;
}

static bool trigger_high(void) {
  uint32_t v = 0;

  pi_gpio_pin_read(cfg.pad_trigger, &v);
  return v != 0;
}

/* After a transaction: the nRF5340 needs a moment to drop the line, then to arm again. */
static int wait_armed(void) {
  uint32_t waited_us = 0;

  pi_time_wait_us(G9L_TURN_US);
  while (!trigger_high()) {
    if (waited_us > G9L_ARM_TIMEOUT_MS * 1000u) {
      return -1;
    }
    pi_time_wait_us(50);
    waited_us += 50;
  }
  return 0;
}

void g9l_wait_work(void) {
  while (xSemaphoreTake(trigger_sem, 0) == pdTRUE) {
  }
  while (!trigger_high()) {
    xSemaphoreTake(trigger_sem, portMAX_DELAY);
  }
}

/* One push: the frame goes out on MOSI, what comes back on MISO is ignored. */
static int push_frame(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len) {
  size_t n = sensei_spi_frame_encode(type, flags, payload, len, tx_frame, sizeof(tx_frame));

  if (n == 0) {
    return -1;
  }
  cs(0);
  pi_time_wait_us(G9L_CS_SETUP_US);
  pi_spi_send(&spi_dev, tx_frame, (uint32_t)n * 8, PI_SPI_CS_NONE);
  pi_time_wait_us(G9L_CS_SETUP_US);
  cs(1);
  return 0;
}

/*
 * One poll. Reads the 8 byte header of the armed frame, and the payload if want_payload and the
 * header is good. Without want_payload the transaction is a PEEK: the nRF5340 does not take the
 * frame. Returns 0 (header good), -1 (header bad). *hdr is filled when the header parsed.
 * The ERROR flag in the header only says that the nRF5340 rejected our last push: the payload is
 * read anyway.
 */
static int poll_frame(bool want_payload, bool report_error, sensei_spi_header_t *hdr, int *payload_bad) {
  int rc = 0;

  *payload_bad = 0;
  sensei_spi_header_pack(poll_hdr, want_payload ? G9L_TYPE_POLL : G9L_TYPE_PEEK, report_error ? G9L_FLAG_ERROR : 0,
                         NULL, 0);
  cs(0);
  pi_time_wait_us(G9L_CS_SETUP_US);
  pi_spi_transfer(&spi_dev, poll_hdr, rx_frame, G9L_HDR * 8, PI_SPI_CS_NONE);
  if (sensei_spi_header_parse(rx_frame, hdr) != SENSEI_SPI_OK) {
    rc = -1;
  } else if (want_payload && hdr->len > 0) {
    pi_spi_receive(&spi_dev, rx_frame + G9L_HDR, (uint32_t)hdr->len * 8, PI_SPI_CS_NONE);
    *payload_bad = sensei_spi_check_payload(hdr, rx_frame + G9L_HDR) != SENSEI_SPI_OK;
  } else if (want_payload && hdr->len == 0) {
    *payload_bad = sensei_spi_check_payload(hdr, rx_frame + G9L_HDR) != SENSEI_SPI_OK;
  }
  pi_time_wait_us(G9L_CS_SETUP_US);
  cs(1);
  return rc;
}

/* Push a frame and check the acknowledgement: the next header of the nRF5340 must not have ERROR.
 */
static int push_acked(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len) {
  for (int tries = 0; tries < G9L_RETRIES; tries++) {
    sensei_spi_header_t h;
    int bad;

    if (wait_armed() || push_frame(type, flags, payload, len)) {
      continue;
    }
    if (wait_armed()) {
      continue;
    }
    if (poll_frame(false, false, &h, &bad) == 0 && !(h.flags & G9L_FLAG_ERROR)) {
      return 0;
    }
    LOG_WRN("push of type 0x%02x not acked, retry\n", type);
  }
  return -1;
}

int g9l_hello(const sensei_Version *v) {
  uint8_t buf[sensei_Version_size];
  int32_t n = sensei_encode_Version(buf, sizeof(buf), v);

  return n < 0 ? -1 : push_acked(G9L_TYPE_HELLO, 0, buf, (size_t)n);
}

int g9l_busfree(uint32_t seq) {
  uint8_t b[4] = {(uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24)};

  return push_acked(G9L_TYPE_BUSFREE, 0, b, sizeof(b));
}

int g9l_send_result(const uint8_t *payload, size_t len, bool more) {
  return push_acked(G9L_TYPE_RESULT, more ? G9L_FLAG_MORE : 0, payload, len);
}

int g9l_fetch(g9l_decode_fn decode, void *job) {
  bool report_error = false;

  for (int tries = 0; tries < G9L_RETRIES; tries++) {
    sensei_spi_header_t h;
    int bad;

    if (wait_armed()) {
      return -2;
    }
    if (poll_frame(true, report_error, &h, &bad) || bad) {
      report_error = true; /* tell the nRF5340 to arm the frame again */
      continue;
    }
    if (h.type == G9L_TYPE_NONE) {
      return G9L_FETCH_NONE;
    }
    if (h.type == G9L_TYPE_JOB && decode(rx_frame + G9L_HDR, h.len, job)) {
      return G9L_FETCH_JOB;
    }
    report_error = true;
  }
  return -1;
}
