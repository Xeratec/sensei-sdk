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
 * GAP9 side of the UART-forwarding example.
 *
 * The GAP9 periodically "senses" a value and emits it as a text line. Its
 * stdout is routed to UART0 (see sdk.config: CONFIG_IO_TYPE_UART=y), and on the
 * SENSEI board UART0 (pads 60/61) is wired to the nRF5340. The nRF receives
 * these bytes on its `uart_gap` instance (uart3) and forwards them verbatim to
 * its default console UART (USB CDC-ACM), where a host can read them.
 *
 * Because stdout is the transport, a plain `printf()` is all that is required
 * on this side, and since this firmware is single-threaded there is no risk of
 * interleaved writes. If you need a dedicated binary protocol or higher
 * throughput, open UART0 explicitly with `pi_open(PI_UART_UART0, &uart)` and
 * use `pi_uart_write()` instead (see `sensei-demo-yolov5/src_GAP/src/uart_comm.c`).
 *
 * IMPORTANT: the UART baud rate must match the nRF side. The nRF `uart_gap`
 * node is configured for 115200 baud (board DTS, `current-speed`), which is
 * also the GAP SDK default for the printf UART. If you change one, change both.
 */

#include "pmsis.h"
#include <stdint.h>
#include <stdio.h>

/* Emit one reading per second. */
#define SENSE_PERIOD_US (1000000)

/*
 * Placeholder "sensor". Replace the body with a real acquisition (I2C/SPI/ADC/
 * camera/...). It returns a deterministic, slowly changing pseudo value so the
 * host sees moving data even without any sensor hardware attached.
 */
static uint32_t read_sensor(uint32_t seq) { return (seq * 7919u + 12345u) % 1000u; }

int main(void) {
  uint32_t seq = 0;

  printf("GAP9 UART forwarding demo started\r\n");

  while (1) {
    uint32_t value = read_sensor(seq);

    /* One CSV line per reading; the trailing newline lets the host (and the
     * nRF, if it wants to frame by line) delimit records easily. */
    printf("SENSOR,seq=%lu,value=%lu\r\n", (unsigned long)seq, (unsigned long)value);

    seq++;
    pi_time_wait_us(SENSE_PERIOD_US);
  }

  return 0;
}
