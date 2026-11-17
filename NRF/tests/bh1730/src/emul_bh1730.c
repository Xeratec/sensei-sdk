/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* I2C emulator of the ROHM BH1730FVC (address 0x29), for the driver "sensei,bh1730".
 *
 * Follows the ROHM datasheet TSZ02201-0M3M0F616050-1-2, Rev.001 (8 Jun 2016): page 7 measurement procedure
 * (power down after power supply; POWER and ADC_EN start a measurement; ONE_TIME ends it in power down),
 * page 8 command set (COMMAND byte: bit 7 = 1, bits 6:5 = 00 register address, 11 special command; invalid
 * addresses must not be used) and register addresses, page 9 CONTROL and TIMING (integration time
 * ITIME_ms = Tint * 964 * (256 - ITIME), measurement time Tmt = ITIME_ms + Tint * 714, Tint = 2.8 us typical),
 * page 10 INTERRUPT, thresholds and GAIN, page 11 PART_ID (0111xxxx) and data registers, page 12 measurement
 * sequence (COMMAND, then CONTROL; COMMAND with the address of DATA0LOW, then four bytes).
 * Registers are read and written with the address counting up after every byte (the sequence on page 12).
 * Modelled: ADC_VALID is set when the measurement time is over and cleared when a data register is read; with
 * ONE_TIME the sensor goes to power down after the measurement (POWER and ADC_EN read back 0; the data stay
 * readable: an interpretation of "ADC transits to power down automatically"); the software reset special command
 * (COMMAND 0xE4). The result registers hold DATA0 and DATA1 from the test controls at the end of a measurement.
 * Not modelled: interrupt function, manual integration mode (ITIME 0), continuous mode restarts. */

#define DT_DRV_COMPAT sensei_bh1730

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>

#include "emul_bh1730.h"

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one BH1730 node");

#define REG_CONTROL 0x00
#define REG_TIMING 0x01
#define REG_GAIN 0x07
#define REG_ID 0x12
#define REG_DATA0LOW 0x14
#define REG_DATA1HIGH 0x17
#define CTL_POWER 0x01
#define CTL_ADC_EN 0x02
#define CTL_ONE_TIME 0x08
#define CTL_VALID 0x10
#define TINT_NS 2800

static struct bh1730_emul st;
static uint8_t cur; /* register address of the next byte (counts up) */
static bool measuring;
static int64_t done_at_us;
static bool valid; /* ADC_VALID */
static uint16_t result0, result1;

struct bh1730_emul *bh1730_emul_get(void) { return &st; }

static int64_t now_us(void) { return k_ticks_to_us_floor64(k_uptime_ticks()); }

uint32_t bh1730_emul_meas_us(uint8_t itime) {
  uint64_t itime_ns = (uint64_t)TINT_NS * 964 * (256 - itime);

  return (uint32_t)((itime_ns + (uint64_t)TINT_NS * 714) / 1000);
}

static void regs_reset(void) {
  memset(st.reg, 0, sizeof(st.reg));
  st.reg[REG_TIMING] = 0xDA;
  st.reg[0x05] = 0xFF;
  st.reg[0x06] = 0xFF;
  measuring = false;
  valid = false;
  result0 = result1 = 0;
  cur = 0;
}

void bh1730_emul_reset(void) {
  memset(&st, 0, sizeof(st));
  st.present = true;
  st.part_id = 0x7B;
  st.fail_after = -1;
  regs_reset();
}

/* Finish a measurement whose time is over. */
static void settle(void) {
  if (measuring && !st.never_valid && now_us() >= done_at_us) {
    measuring = false;
    valid = true;
    result0 = st.data0;
    result1 = st.data1;
    if (st.reg[REG_CONTROL] & CTL_ONE_TIME) {
      st.reg[REG_CONTROL] &= (uint8_t)~(CTL_POWER | CTL_ADC_EN);
    }
  }
}

uint8_t bh1730_emul_control(void) {
  settle();
  return (uint8_t)((st.reg[REG_CONTROL] & 0x0F) | (valid ? CTL_VALID : 0));
}

static bool valid_addr(uint8_t a) { return a <= REG_GAIN || a == REG_ID || (a >= REG_DATA0LOW && a <= REG_DATA1HIGH); }

static void log_write(uint8_t reg, uint8_t val) {
  if (st.n_writes < BH1730_EMUL_LOG_LEN) {
    st.writes[st.n_writes++] = (reg << 8) | val;
  }
}

static void write_reg(uint8_t reg, uint8_t val) {
  log_write(reg, val);
  if (reg == REG_ID || (reg >= REG_DATA0LOW && reg <= REG_DATA1HIGH)) {
    return; /* read only */
  }
  if (reg == REG_CONTROL) {
    settle();
    st.reg[REG_CONTROL] = val & 0x0F; /* ADC_INTR and ADC_VALID are read only */
    if ((val & CTL_POWER) && (val & CTL_ADC_EN)) {
      if (!measuring) {
        measuring = true;
        valid = false;
        st.measurements++;
        done_at_us = now_us() + bh1730_emul_meas_us(st.reg[REG_TIMING]);
      }
    } else if (!(val & CTL_POWER)) {
      measuring = false;
    }
    return;
  }
  st.reg[reg] = val;
}

static uint8_t read_reg(uint8_t reg) {
  settle();
  switch (reg) {
  case REG_CONTROL:
    return bh1730_emul_control();
  case REG_ID:
    return st.part_id;
  case REG_DATA0LOW:
    return (uint8_t)result0;
  case REG_DATA0LOW + 1:
    valid = false;
    return (uint8_t)(result0 >> 8);
  case REG_DATA0LOW + 2:
    return (uint8_t)result1;
  case REG_DATA1HIGH:
    valid = false;
    return (uint8_t)(result1 >> 8);
  default:
    return st.reg[reg];
  }
}

static int nack(void) {
  st.nacks++;
  return -EIO;
}

static int do_write(const uint8_t *buf, size_t len, bool first) {
  size_t i = 0;

  if (first) {
    uint8_t c = buf[0];

    if (len == 0 || !(c & 0x80)) {
      return nack();
    }
    if (((c >> 5) & 3) == 3) { /* special command */
      if ((c & 0x1F) == 0x04) {
        regs_reset();
        return len == 1 ? 0 : nack();
      }
      return nack(); /* the others belong to modes that are not modelled */
    }
    if (((c >> 5) & 3) != 0 || !valid_addr(c & 0x1F)) {
      return nack();
    }
    cur = c & 0x1F;
    i = 1;
  }
  for (; i < len; i++) {
    if (!valid_addr(cur)) {
      return nack();
    }
    write_reg(cur++, buf[i]);
  }
  return 0;
}

static int do_read(uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (!valid_addr(cur)) {
      return nack();
    }
    buf[i] = read_reg(cur++);
  }
  return 0;
}

static int bh1730_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr) {
  ARG_UNUSED(target);
  st.transfers++;
  if (!st.present || addr != DT_INST_REG_ADDR(0)) {
    return nack();
  }
  if (st.fail_after >= 0 && st.transfers > st.fail_after) {
    return nack();
  }
  for (int i = 0; i < num_msgs; i++) {
    int ret;

    if (msgs[i].flags & I2C_MSG_READ) {
      ret = do_read(msgs[i].buf, msgs[i].len);
    } else {
      /* the first write message of a transfer starts with the COMMAND byte */
      ret = do_write(msgs[i].buf, msgs[i].len, i == 0 || (msgs[i - 1].flags & I2C_MSG_READ));
    }
    if (ret != 0) {
      return ret;
    }
  }
  return 0;
}

static const struct i2c_emul_api bh1730_emul_api = {
    .transfer = bh1730_emul_transfer,
};

static int bh1730_emul_init(const struct emul *target, const struct device *parent) {
  ARG_UNUSED(target);
  ARG_UNUSED(parent);
  bh1730_emul_reset();
  return 0;
}

#define BH1730_EMUL(n) EMUL_DT_INST_DEFINE(n, bh1730_emul_init, NULL, NULL, &bh1730_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(BH1730_EMUL)
