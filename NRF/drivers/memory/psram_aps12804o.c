/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * APS12804O PSRAM on the nRF5340 QSPI peripheral, used in SPI mode with the quad commands
 * (fast quad read 0xEB, quad write 0x38). Custom instructions send reset, read ID and half-sleep.
 *
 * - The QSPI peripheral wants word-aligned buffers and lengths, the PSRAM does not: every access goes
 *   through a small aligned bounce buffer.
 * - CE# must not stay low longer than tCEM (8 us, 3 us at extended temperature), so transfers are
 *   cut into 32 byte pieces (about 2.6 us at 32 MHz).
 * Untested on hardware.
 */

#define DT_DRV_COMPAT apmemory_aps12804o

#include <errno.h>
#include <hal/nrf_gpio.h>
#include <nrfx_qspi.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <sensei/drivers/psram_aps12804o.h>

LOG_MODULE_REGISTER(psram_aps12804o, CONFIG_PSRAM_APS12804O_LOG_LEVEL);

#define CMD_RESET_ENABLE 0x66
#define CMD_RESET 0x99
#define CMD_HALF_SLEEP 0xC0
#define CMD_READ_ID 0x9F
#define ID_MF 0x0D
#define ID_KGD 0x5D

#define CHUNK 32U
/* Supply ramp of the load switch plus the 150 us the PSRAM needs after VDD is stable. */
#define POWER_UP_MS 2
#define T_HS_US 150
#define T_XHS_US 150

/* The nRF5340 QSPI uses dedicated pins. */
#define PIN_IO0 NRF_GPIO_PIN_MAP(0, 13)
#define PIN_IO1 NRF_GPIO_PIN_MAP(0, 14)
#define PIN_IO2 NRF_GPIO_PIN_MAP(0, 15)
#define PIN_IO3 NRF_GPIO_PIN_MAP(0, 16)
#define PIN_SCK NRF_GPIO_PIN_MAP(0, 17)
#define PIN_CSN NRF_GPIO_PIN_MAP(0, 18)

struct psram_config {
  struct gpio_dt_spec power;
  struct gpio_dt_spec cs_select;
};

struct psram_data {
  struct k_mutex lock;
  bool powered;
  bool qspi_up;
  bool sleeping;
  uint8_t bounce[CHUNK + 8] __aligned(4);
};

static int cinstr(uint8_t opcode, nrf_qspi_cinstr_len_t len, const void *tx, void *rx) {
  /* IO2 and IO3 high: in SPI mode they are WP# and HOLD# on some parts. */
  nrf_qspi_cinstr_conf_t c = {.opcode = opcode, .length = len, .io2_level = true, .io3_level = true};

  return nrfx_qspi_cinstr_xfer(&c, tx, rx);
}

static int qspi_start(void) {
  const nrfx_qspi_config_t cfg = {
      .xip_offset = 0,
      .pins =
          {
              .sck_pin = PIN_SCK,
              .csn_pin = PIN_CSN,
              .io0_pin = PIN_IO0,
              .io1_pin = PIN_IO1,
              .io2_pin = PIN_IO2,
              .io3_pin = PIN_IO3,
          },
      .prot_if =
          {
              .readoc = NRF_QSPI_READOC_READ4IO, /* 0xEB, 6 wait cycles */
              .writeoc = NRF_QSPI_WRITEOC_PP4IO, /* 0x38 */
              .addrmode = NRF_QSPI_ADDRMODE_24BIT,
              .dpmconfig = false,
          },
      .phy_if =
          {
              .sck_delay = 1,
              .dpmen = false,
              .spi_mode = NRF_QSPI_MODE_0,
              .sck_freq = NRF_QSPI_FREQ_DIV3, /* 96 MHz / 3 */
          },
      .timeout = 10,
      .irq_priority = 7,
  };

  /* Blocking mode: no handler, no interrupt. */
  return nrfx_qspi_init(&cfg, NULL, NULL);
}

int psram_aps12804o_power_on(const struct device *dev) {
  const struct psram_config *cfg = dev->config;
  struct psram_data *d = dev->data;
  uint8_t tx[5] = {0};
  uint8_t rx[5] = {0};
  int ret;

  k_mutex_lock(&d->lock, K_FOREVER);
  if (d->powered) {
    ret = 0;
    goto out;
  }
  /* CS to the PSRAM before the supply comes up, so the line does not float. */
  gpio_pin_set_dt(&cfg->cs_select, 0);
  gpio_pin_set_dt(&cfg->power, 1);
  k_msleep(POWER_UP_MS);

  ret = qspi_start();
  if (ret != 0) {
    LOG_ERR("QSPI init failed (%d)", ret);
    goto off;
  }
  d->qspi_up = true;
  ret = cinstr(CMD_RESET_ENABLE, NRF_QSPI_CINSTR_LEN_1B, NULL, NULL);
  if (ret == 0) {
    ret = cinstr(CMD_RESET, NRF_QSPI_CINSTR_LEN_1B, NULL, NULL);
  }
  if (ret != 0) {
    LOG_ERR("reset failed (%d), the QSPI start-up status read may time out on a PSRAM", ret);
    goto off;
  }
  k_busy_wait(1);
  /* Read ID: opcode, 3 address bytes, then MF ID and KGD. Only valid right after the reset. */
  ret = cinstr(CMD_READ_ID, NRF_QSPI_CINSTR_LEN_6B, tx, rx);
  if (ret != 0 || rx[3] != ID_MF || rx[4] != ID_KGD) {
    LOG_ERR("unexpected ID %02x %02x (%d)", rx[3], rx[4], ret);
    ret = ret ? ret : -ENODEV;
    goto off;
  }
  d->powered = true;
  d->sleeping = false;
  goto out;

off:
  if (d->qspi_up) {
    nrfx_qspi_uninit();
    d->qspi_up = false;
  }
  gpio_pin_set_dt(&cfg->power, 0);
out:
  k_mutex_unlock(&d->lock);
  return ret;
}

/* One access of at most CHUNK bytes inside one 32 byte block, any alignment. */
static int piece(struct psram_data *d, uint32_t addr, uint8_t *rd, const uint8_t *wr, size_t n) {
  uint32_t a0 = addr & ~3U;
  size_t lead = addr - a0;
  size_t len = ROUND_UP(lead + n, 4U);
  int ret;

  if (rd != NULL) {
    ret = nrfx_qspi_read(d->bounce, len, a0);
    if (ret == 0) {
      memcpy(rd, &d->bounce[lead], n);
    }
    return ret;
  }
  if (lead != 0 || (n & 3U) != 0) {
    /* Partial words at the edges: keep the neighbouring bytes. */
    ret = nrfx_qspi_read(d->bounce, len, a0);
    if (ret != 0) {
      return ret;
    }
  }
  memcpy(&d->bounce[lead], wr, n);
  return nrfx_qspi_write(d->bounce, len, a0);
}

static int access(const struct device *dev, uint32_t addr, uint8_t *rd, const uint8_t *wr, size_t len) {
  struct psram_data *d = dev->data;
  int ret = 0;

  if ((uint64_t)addr + len > PSRAM_APS12804O_SIZE) {
    return -EINVAL;
  }
  k_mutex_lock(&d->lock, K_FOREVER);
  if (!d->powered || d->sleeping) {
    ret = -EACCES;
    goto out;
  }
  while (len > 0 && ret == 0) {
    size_t n = MIN(len, CHUNK - (addr % CHUNK));

    ret = piece(d, addr, rd, wr, n);
    addr += n;
    len -= n;
    if (rd) {
      rd += n;
    } else {
      wr += n;
    }
  }
out:
  k_mutex_unlock(&d->lock);
  return ret;
}

int psram_aps12804o_read(const struct device *dev, uint32_t addr, void *buf, size_t len) {
  return access(dev, addr, buf, NULL, len);
}

int psram_aps12804o_write(const struct device *dev, uint32_t addr, const void *buf, size_t len) {
  return access(dev, addr, NULL, buf, len);
}

int psram_aps12804o_half_sleep(const struct device *dev) {
  struct psram_data *d = dev->data;
  int ret = -EACCES;

  k_mutex_lock(&d->lock, K_FOREVER);
  if (d->powered && !d->sleeping) {
    ret = cinstr(CMD_HALF_SLEEP, NRF_QSPI_CINSTR_LEN_1B, NULL, NULL);
    if (ret == 0) {
      k_busy_wait(T_HS_US);
      /* Stop the QSPI clocks. The pins keep their level, CE# stays high. */
      nrfx_qspi_deactivate();
      d->sleeping = true;
    }
  }
  k_mutex_unlock(&d->lock);
  return ret;
}

int psram_aps12804o_wake(const struct device *dev) {
  struct psram_data *d = dev->data;
  int ret = -EACCES;

  k_mutex_lock(&d->lock, K_FOREVER);
  if (d->powered && d->sleeping) {
    ret = nrfx_qspi_activate(false);
    if (ret == 0) {
      /*
       * A low pulse on CE# wakes the PSRAM. A one byte instruction does that. 0x66 is harmless: it only
       * arms the reset, and the next command that is not 0x99 clears it.
       */
      ret = cinstr(CMD_RESET_ENABLE, NRF_QSPI_CINSTR_LEN_1B, NULL, NULL);
      k_busy_wait(T_XHS_US);
      d->sleeping = ret != 0;
    }
  }
  k_mutex_unlock(&d->lock);
  return ret;
}

int psram_aps12804o_power_off(const struct device *dev) {
  const struct psram_config *cfg = dev->config;
  struct psram_data *d = dev->data;

  k_mutex_lock(&d->lock, K_FOREVER);
  if (d->qspi_up) {
    nrfx_qspi_uninit();
    d->qspi_up = false;
  }
  gpio_pin_set_dt(&cfg->power, 0);
  d->powered = false;
  d->sleeping = false;
  k_mutex_unlock(&d->lock);
  return 0;
}

static int psram_init(const struct device *dev) {
  const struct psram_config *cfg = dev->config;
  struct psram_data *d = dev->data;
  int ret;

  k_mutex_init(&d->lock);
  if (!gpio_is_ready_dt(&cfg->power) || !gpio_is_ready_dt(&cfg->cs_select)) {
    return -ENODEV;
  }
  /* Supply off and CS on the PSRAM until the application asks for it. This runs at the device priority (50), before
   * the PMIC reset of the power module (POST_KERNEL 55) on purpose: the supply switch is an nRF pin, and it must be
   * off while the PMIC cycles the rails. */
  ret = gpio_pin_configure_dt(&cfg->power, GPIO_OUTPUT_INACTIVE);
  if (ret == 0) {
    ret = gpio_pin_configure_dt(&cfg->cs_select, GPIO_OUTPUT_INACTIVE);
  }
  return ret;
}

#define PSRAM_INIT(inst)                                                                                               \
  static struct psram_data psram_data_##inst;                                                                          \
  static const struct psram_config psram_cfg_##inst = {                                                                \
      .power = GPIO_DT_SPEC_INST_GET(inst, power_gpios),                                                               \
      .cs_select = GPIO_DT_SPEC_INST_GET(inst, cs_select_gpios),                                                       \
  };                                                                                                                   \
  DEVICE_DT_INST_DEFINE(inst, psram_init, NULL, &psram_data_##inst, &psram_cfg_##inst, POST_KERNEL,                    \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(PSRAM_INIT)
