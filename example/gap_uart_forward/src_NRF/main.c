/*
 * ----------------------------------------------------------------------
 *
 * File: main.c
 *
 * Last edited: 11.06.2026
 *
 * Copyright (c) 2024 ETH Zurich and University of Bologna
 *
 * Authors:
 * - Philip Wiese (wiesep@iis.ee.ethz.ch), ETH Zurich
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * nRF side of the UART-forwarding example.
 *
 * Data path:
 *
 *     GAP9 stdout --UART0--> nRF uart_gap (uart3) --copy--> USB CDC-ACM --> host
 *
 * The nRF receives bytes from the GAP9 on `uart_gap` (uart3, 115200 baud,
 * P1.00/P1.01 - see board DTS) using the asynchronous (DMA-backed) UART API,
 * and forwards them verbatim to its default console UART, which on this board
 * is the USB CDC-ACM device (`chosen { zephyr,console = &cdc_acm_uart0; }`).
 *
 * Design notes (robustness):
 *  - Reception is *continuous*: two static buffers are handed to the driver in
 *    a strict ping-pong, so no bytes are lost between buffer swaps and there is
 *    no dependence on any framing/terminator character.
 *  - The RX callback only copies the received slice into a lock-free ring
 *    buffer and signals a worker thread; it never blocks.
 *  - A dedicated thread drains the ring buffer to the USB console. Forwarding
 *    is gated on the CDC-ACM DTR line so that, when no host is attached, bytes
 *    are discarded instead of stalling the pipeline.
 *  - Ring-buffer overflows are counted and logged rather than silently dropped.
 *  - The nRF's own log output goes to RTT (see prj.conf), keeping the USB
 *    console a clean, dedicated channel for the forwarded GAP9 data.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/usb/usb_device.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <SEGGER_RTT.h>

#include "bsp/pwr_bsp.h"
#include "pwr/pwr.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define GPIO_NODE_gap9_spi_ctrl DT_NODELABEL(gpio_gap9_spi_ctrl)
#define GPIO_NODE_gap9_shared_ctrl DT_NODELABEL(gpio_gap9_shared_ctrl)

/* UART connected to the GAP9 (uart3 on the SENSEI board). */
#define UART_GAP_NODE DT_NODELABEL(uart_gap)
static const struct device *const uart_gap = DEVICE_DT_GET(UART_GAP_NODE);

/* Default console UART of the nRF = USB CDC-ACM. */
static const struct device *const uart_usb = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

static const struct gpio_dt_spec gpio_gap9_spi_ctrl = GPIO_DT_SPEC_GET(GPIO_NODE_gap9_spi_ctrl, gpios);
static const struct gpio_dt_spec gpio_gap9_shared_ctrl = GPIO_DT_SPEC_GET(GPIO_NODE_gap9_shared_ctrl, gpios);

/* Asynchronous RX double buffer. RX_CHUNK_SIZE bounds the worst-case latency
 * (a buffer is released either when full or after RX_TIMEOUT_US of line idle). */
#define RX_CHUNK_SIZE 64
#define RX_TIMEOUT_US 1000000 /* flush a partial buffer after 10 ms of idle line */
static uint8_t rx_buf[2][RX_CHUNK_SIZE];
static uint8_t rx_buf_next; /* index of the buffer to hand to the driver next */

/* Byte pipe: RX callback (producer) -> forwarding thread (consumer). The
 * Zephyr ring buffer is lock-free for a single producer and single consumer. */
#define FWD_RING_SIZE 2048
RING_BUF_DECLARE(fwd_ring, FWD_RING_SIZE);
static K_SEM_DEFINE(fwd_sem, 0, 1);
static atomic_t dropped_bytes = ATOMIC_INIT(0);

static void uart_gap_cb(const struct device *dev, struct uart_event *evt, void *user_data) {
  ARG_UNUSED(user_data);

  switch (evt->type) {
  case UART_RX_RDY: {
    /* New bytes are available at buf + offset for `len` bytes. */
    const uint8_t *data = &evt->data.rx.buf[evt->data.rx.offset];
    uint32_t put = ring_buf_put(&fwd_ring, data, evt->data.rx.len);

    if (put < evt->data.rx.len) {
      atomic_add(&dropped_bytes, (atomic_val_t)(evt->data.rx.len - put));
    }
    k_sem_give(&fwd_sem);
    break;
  }

  case UART_RX_BUF_REQUEST:
    /* Provide the spare buffer so reception continues without interruption. */
    uart_rx_buf_rsp(dev, rx_buf[rx_buf_next], RX_CHUNK_SIZE);
    rx_buf_next ^= 1U;
    break;

  case UART_RX_DISABLED:
    /* Reached after an error/stop; restart reception for resilience. Logging
     * is intentionally avoided here because this callback runs in ISR context. */
    rx_buf_next = 1U;
    uart_rx_enable(dev, rx_buf[0], RX_CHUNK_SIZE, RX_TIMEOUT_US);
    break;

  case UART_RX_STOPPED:
    /* Line error (e.g. framing/overrun). The driver follows up with
     * UART_RX_DISABLED, where reception is restarted. */
    break;

  default:
    break;
  }
}

static void forward_thread(void) {
  uint8_t buf[RX_CHUNK_SIZE];
  uint32_t n;

  for (;;) {
    k_sem_take(&fwd_sem, K_FOREVER);

    atomic_val_t dropped = atomic_set(&dropped_bytes, 0);
    if (dropped) {
      LOG_WRN("Forward ring buffer overflow: dropped %ld byte(s)", (long)dropped);
    }

    /* Is a host attached to the USB CDC-ACM port? (DTR asserted) */
    uint32_t dtr = 0;
    bool usb_ready = (uart_line_ctrl_get(uart_usb, UART_LINE_CTRL_DTR, &dtr) == 0) && (dtr != 0);

    while ((n = ring_buf_get(&fwd_ring, buf, sizeof(buf))) > 0) {
      /* Sink 1 - SEGGER RTT up-channel 0. Non-blocking: if the host viewer is
       * not draining the channel the bytes are dropped, so RTT can never stall
       * the forwarder. This is the same channel the log/console viewer uses. */
      SEGGER_RTT_Write(0, buf, n);

      /* Sink 2 - USB CDC-ACM. Only write when a host has opened the port,
       * otherwise uart_poll_out() can block on a full TX buffer with no reader. */
      if (usb_ready) {
        for (uint32_t i = 0; i < n; i++) {
          uart_poll_out(uart_usb, buf[i]);
        }
      }
    }
  }
}

K_THREAD_DEFINE(forward_thread_id, 1024, forward_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void) {
  LOG_INIT();

  LOG_INF("GAP9 -> nRF UART forwarding demo on %s", CONFIG_BOARD);

  /* Power management (also brings up the GAP9 power rail). */
  if (pwr_init()) {
    LOG_ERR("PWR init failed");
  }
  pwr_start();

  if (!device_is_ready(uart_usb)) {
    LOG_ERR("USB CDC-ACM device not ready");
    return 0;
  }

  if (!device_is_ready(uart_gap)) {
    LOG_ERR("GAP UART device not ready");
    return 0;
  }

  if (usb_enable(NULL)) {
    LOG_ERR("USB enable failed");
    return 0;
  }
  LOG_INF("USB console enabled");

  if (uart_callback_set(uart_gap, uart_gap_cb, NULL)) {
    LOG_ERR("GAP UART callback configuration failed");
    return 0;
  }

  // Enable shared GPIOs (also required for nRF <-> GAP UART)
  if (gpio_pin_set_dt(&gpio_gap9_shared_ctrl, 1) < 0) {
    LOG_ERR("GAP9 Shared GPIO configuration error");
    return 0;
  } else {
    LOG_INF("GAP9 Shared GPIOs enabled");
  }

  // Required to allow overwriting the boot mode of GAP9 via the debug PCB
  if (gpio_pin_set_dt(&gpio_gap9_spi_ctrl, 1) < 0) {
    LOG_ERR("GAP9 SPI GPIO configuration error");
    return 0;
  } else {
    LOG_INF("GAP9 connected to SPI bus");
  }

  k_msleep(100);

  gap9_pwr(true);
  LOG_INF("GAP9 powered up");

  /* Start continuous reception on the GAP UART. */
  rx_buf_next = 1U;
  if (uart_rx_enable(uart_gap, rx_buf[0], RX_CHUNK_SIZE, RX_TIMEOUT_US)) {
    LOG_ERR("Failed to enable GAP UART reception");
    return 0;
  }
  LOG_INF("Forwarding GAP UART -> USB console");

  for (;;) {
    k_msleep(1000);
  }

  return 0;
}
