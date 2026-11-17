/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 * - Serge Wüest <swueest@ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

/**
 * @file lr1121_zephyr_hal.c
 * @brief Zephyr implementation of the Semtech LR1121 Modem-E host HAL (lr1121_modem_hal.h),
 *        based on devicetree specs (SPI bus, chip select, reset, busy). Replaces the HAL of the
 *        Semtech sample.
 *
 * Changes against the commissioning version: devicetree specs from the lr1121 node, no k_oops()
 * when BUSY hangs (the error goes up and the service recovers), the SPI mutex is shared with the
 * service, short waits use k_usleep().
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lr1121_modem_hal.h"
#include "lr1121_zephyr_hal.h"

LOG_MODULE_REGISTER(lr1121_hal, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

#define LR_NODE DT_ALIAS(lr1121)
#define LR_BUS DT_BUS(LR_NODE)

K_MUTEX_DEFINE(lr1121_spi_mutex);

struct lr1121_context lr1121_ctx = {
    .spi_gpio =
        {
            .spi =
                {
                    .bus = DEVICE_DT_GET(LR_BUS),
                    .config =
                        {
                            .frequency = DT_PROP(LR_NODE, spi_max_frequency),
                            .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER,
                            .slave = 0,
                            /* no .cs: the chip select is driven here, see below */
                        },
                },
            .reset = GPIO_DT_SPEC_GET(LR_NODE, reset_gpios),
            .busy = GPIO_DT_SPEC_GET(LR_NODE, busy_gpios),
            .event = GPIO_DT_SPEC_GET(LR_NODE, event_gpios),
            .cs = GPIO_DT_SPEC_GET_BY_IDX(LR_BUS, cs_gpios, 0),
        },
    .status = {.radio_status = RADIO_AWAKE},
};

int lr1121_hal_init(void) {
  struct lr1121_spi_gpio *c = &lr1121_ctx.spi_gpio;

  if (!spi_is_ready_dt(&c->spi) || !gpio_is_ready_dt(&c->reset) || !gpio_is_ready_dt(&c->busy) ||
      !gpio_is_ready_dt(&c->event) || !gpio_is_ready_dt(&c->cs)) {
    return -ENODEV;
  }
  /* Hold the radio in reset until the service starts it. */
  int err = gpio_pin_configure_dt(&c->reset, GPIO_OUTPUT_ACTIVE);

  err |= gpio_pin_configure_dt(&c->cs, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&c->busy, GPIO_INPUT);
  err |= gpio_pin_configure_dt(&c->event, GPIO_INPUT);
  return err ? -EIO : 0;
}

static int wait_on_busy(const struct lr1121_spi_gpio *c, bool level) {
  int64_t end = k_uptime_get() + LORA_BASICS_MODEM_DRIVERS_HAL_WAIT_ON_BUSY_TIMEOUT_MSEC;

  while (k_uptime_get() <= end) {
    if ((gpio_pin_get_dt(&c->busy) > 0) == level) {
      return 0;
    }
    k_usleep(100);
  }
  LOG_ERR("LR1121 BUSY timeout (%d ms)", LORA_BASICS_MODEM_DRIVERS_HAL_WAIT_ON_BUSY_TIMEOUT_MSEC);
  return -ETIMEDOUT;
}

/* The radio is ready when BUSY is low. In sleep BUSY is high: wake it with a pulse on NSS. */
static int check_device_ready(struct lr1121_context *ctx) {
  const struct lr1121_spi_gpio *c = &ctx->spi_gpio;

  if (ctx->status.radio_status != RADIO_SLEEP) {
    return wait_on_busy(c, false);
  }
  gpio_pin_set_dt(&c->cs, 1);
  k_busy_wait(100);
  gpio_pin_set_dt(&c->cs, 0);
  ctx->status.radio_status = RADIO_AWAKE;
  return wait_on_busy(c, false);
}

static int spi_out(struct lr1121_context *ctx, const struct spi_buf_set *tx, const struct spi_buf_set *rx) {
  const struct lr1121_spi_gpio *c = &ctx->spi_gpio;
  int ret;

  gpio_pin_set_dt(&c->cs, 1);
  ret = rx ? spi_transceive_dt(&c->spi, tx, rx) : spi_write_dt(&c->spi, tx);
  gpio_pin_set_dt(&c->cs, 0);
  return ret;
}

static bool is_sleep_cmd(const uint8_t *command, uint16_t command_length) {
  /* LR1121_SYSTEM_SET_SLEEP_OC = 0x011B */
  return command_length > 1 && command[0] == 0x01 && command[1] == 0x1B;
}

lr1121_modem_hal_status_t lr1121_modem_hal_write(const void *context, const uint8_t *command,
                                                 const uint16_t command_length, const uint8_t *data,
                                                 const uint16_t data_length) {
  struct lr1121_context *ctx = (struct lr1121_context *)context;
  uint8_t return_code = 0, rx_crc = 0;
  uint8_t crc = lr1121_modem_compute_crc(0xFF, command, command_length);

  crc = lr1121_modem_compute_crc(crc, data, data_length);

  const struct spi_buf tx_buf[] = {{.buf = (uint8_t *)command, .len = command_length},
                                   {.buf = (uint8_t *)data, .len = data_length},
                                   {.buf = &crc, .len = 1}};
  const struct spi_buf_set tx = {.buffers = tx_buf, .count = ARRAY_SIZE(tx_buf)};
  uint8_t nop[2] = {0, 0};
  const struct spi_buf nop_buf = {.buf = nop, .len = sizeof(nop)};
  const struct spi_buf_set tx_nop = {.buffers = &nop_buf, .count = 1};
  const struct spi_buf rx_buf[] = {{.buf = &return_code, .len = 1}, {.buf = &rx_crc, .len = 1}};
  const struct spi_buf_set rx = {.buffers = rx_buf, .count = ARRAY_SIZE(rx_buf)};
  lr1121_modem_hal_status_t st = LR1121_MODEM_HAL_STATUS_ERROR;

  k_mutex_lock(&lr1121_spi_mutex, K_FOREVER);
  k_busy_wait(200); /* let the radio raise BUSY after an event */
  if (check_device_ready(ctx)) {
    st = LR1121_MODEM_HAL_STATUS_BUSY_TIMEOUT;
    goto out;
  }
  if (spi_out(ctx, &tx, NULL)) {
    LOG_ERR("SPI write failed");
    goto out;
  }
  if (wait_on_busy(&ctx->spi_gpio, false)) {
    st = LR1121_MODEM_HAL_STATUS_BUSY_TIMEOUT;
    goto out;
  }
  if (spi_out(ctx, &tx_nop, &rx)) {
    LOG_ERR("SPI response read failed");
    goto out;
  }
  if (rx_crc != lr1121_modem_compute_crc(0xFF, &return_code, 1)) {
    LOG_ERR("CRC of the write response is wrong");
    goto out;
  }
  if (is_sleep_cmd(command, command_length)) {
    ctx->status.radio_status = RADIO_SLEEP;
    k_usleep(500); /* do not wake it before it is asleep */
  }
  st = (lr1121_modem_hal_status_t)return_code;
out:
  k_mutex_unlock(&lr1121_spi_mutex);
  return st;
}

lr1121_modem_hal_status_t lr1121_modem_hal_read(const void *context, const uint8_t *command,
                                                const uint16_t command_length, uint8_t *data,
                                                const uint16_t data_length) {
  struct lr1121_context *ctx = (struct lr1121_context *)context;
  uint8_t tx_data[command_length + 1];
  uint8_t nop[1 + data_length + 1];
  uint8_t rx_data[1 + data_length + 1];
  uint8_t return_code, rx_crc, computed;
  lr1121_modem_hal_status_t st = LR1121_MODEM_HAL_STATUS_ERROR;

  memcpy(tx_data, command, command_length);
  tx_data[command_length] = lr1121_modem_compute_crc(0xFF, command, command_length);
  memset(nop, 0, sizeof(nop));
  memset(rx_data, 0, sizeof(rx_data));

  const struct spi_buf tx_buf = {.buf = tx_data, .len = sizeof(tx_data)};
  const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
  const struct spi_buf nop_buf = {.buf = nop, .len = sizeof(nop)};
  const struct spi_buf_set tx_nop = {.buffers = &nop_buf, .count = 1};
  const struct spi_buf rx_buf = {.buf = rx_data, .len = sizeof(rx_data)};
  const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

  k_mutex_lock(&lr1121_spi_mutex, K_FOREVER);
  if (command_length > 1 && command[0] == 0x05 && command[1] == 0x0B) {
    k_msleep(1); /* crypto restore during start-up: do not hit the radio too early */
  }
  if (check_device_ready(ctx)) {
    st = LR1121_MODEM_HAL_STATUS_BUSY_TIMEOUT;
    goto out;
  }
  if (spi_out(ctx, &tx, NULL)) {
    goto out;
  }
  if (wait_on_busy(&ctx->spi_gpio, false)) {
    st = LR1121_MODEM_HAL_STATUS_BUSY_TIMEOUT;
    goto out;
  }
  if (spi_out(ctx, &tx_nop, &rx)) {
    goto out;
  }
  return_code = rx_data[0];
  if (return_code == 0x00) {
    memcpy(data, &rx_data[1], data_length);
    rx_crc = rx_data[1 + data_length];
  } else {
    rx_crc = rx_data[1]; /* after an error code the CRC follows directly */
    if (data != NULL && data_length > 0) {
      memset(data, 0, data_length);
    }
  }
  computed = lr1121_modem_compute_crc(0xFF, &return_code, 1);
  if (return_code == 0x00) {
    computed = lr1121_modem_compute_crc(computed, data, data_length);
  }
  if (rx_crc != computed) {
    LOG_ERR("CRC of the read response is wrong");
    goto out;
  }
  st = (lr1121_modem_hal_status_t)return_code;
out:
  k_mutex_unlock(&lr1121_spi_mutex);
  return st;
}

lr1121_modem_hal_status_t lr1121_modem_hal_write_without_rc(const void *context, const uint8_t *command,
                                                            const uint16_t command_length, const uint8_t *data,
                                                            const uint16_t data_length) {
  struct lr1121_context *ctx = (struct lr1121_context *)context;
  uint8_t crc = lr1121_modem_compute_crc(0xFF, command, command_length);

  crc = lr1121_modem_compute_crc(crc, data, data_length);

  const struct spi_buf tx_buf[] = {{.buf = (uint8_t *)command, .len = command_length},
                                   {.buf = (uint8_t *)data, .len = data_length},
                                   {.buf = &crc, .len = 1}};
  const struct spi_buf_set tx = {.buffers = tx_buf, .count = ARRAY_SIZE(tx_buf)};
  lr1121_modem_hal_status_t st = LR1121_MODEM_HAL_STATUS_ERROR;

  k_mutex_lock(&lr1121_spi_mutex, K_FOREVER);
  if (check_device_ready(ctx)) {
    st = LR1121_MODEM_HAL_STATUS_BUSY_TIMEOUT;
    goto out;
  }
  if (spi_out(ctx, &tx, NULL)) {
    goto out;
  }
  if (is_sleep_cmd(command, command_length)) {
    ctx->status.radio_status = RADIO_SLEEP;
    k_usleep(500);
  }
  st = LR1121_MODEM_HAL_STATUS_OK;
out:
  k_mutex_unlock(&lr1121_spi_mutex);
  return st;
}

lr1121_modem_hal_status_t lr1121_modem_hal_write_read(const void *context, const uint8_t *command, uint8_t *data,
                                                      const uint16_t data_length) {
  ARG_UNUSED(context);
  ARG_UNUSED(command);
  ARG_UNUSED(data);
  ARG_UNUSED(data_length);
  return LR1121_MODEM_HAL_STATUS_ERROR; /* not used by the Modem-E driver */
}

lr1121_modem_hal_status_t lr1121_modem_hal_reset(const void *context) {
  struct lr1121_context *ctx = (struct lr1121_context *)context;
  const struct lr1121_spi_gpio *c = &ctx->spi_gpio;

  k_mutex_lock(&lr1121_spi_mutex, K_FOREVER);
  gpio_pin_set_dt(&c->reset, 1);
  k_msleep(1);
  gpio_pin_set_dt(&c->reset, 0);
  k_msleep(1);
  ctx->status.radio_status = RADIO_AWAKE;
  k_mutex_unlock(&lr1121_spi_mutex);
  k_msleep(1000); /* the LR1121 firmware needs about a second to start */
  return LR1121_MODEM_HAL_STATUS_OK;
}

void lr1121_modem_hal_enter_dfu(const void *context) {
  ARG_UNUSED(context); /* firmware update is not part of this app, see README */
}

lr1121_modem_hal_status_t lr1121_modem_hal_wakeup(const void *context) {
  struct lr1121_context *ctx = (struct lr1121_context *)context;
  int err;

  k_mutex_lock(&lr1121_spi_mutex, K_FOREVER);
  err = check_device_ready(ctx);
  k_mutex_unlock(&lr1121_spi_mutex);
  return err ? LR1121_MODEM_HAL_STATUS_BUSY_TIMEOUT : LR1121_MODEM_HAL_STATUS_OK;
}
