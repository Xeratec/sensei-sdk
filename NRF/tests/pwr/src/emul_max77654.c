/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* I2C emulator of the Maxim MAX77654 PMIC (address 0x48), enough for NRF/system/pwr and the driver in
 * system/third_party/max77654. Register map and field layout are those of the driver's max77654_reg.h.
 *
 * Modelled:
 *  - a register file with auto-increment, as the driver uses it (one byte per access, burst reads of one);
 *  - the interrupt flag registers INT_GLBL0, INT_GLBL1 and INT_CHG clear on read; the test raises them;
 *  - a soft reset (CNFG_GLBL, SFT_CTRL = SFT_CRST) clears every register, so all rails go off, and the chip does not
 *    answer for PMIC_EMUL_RESET_BUSY_MS (accesses then count as "early": a missing wait of the caller);
 *  - the rails as the registers set them: SBBx voltage 0.8 V + 50 mV * code, LDOx 0.8 V + 25 mV * code (the encoding
 *    of the driver, encode_mV()), on only when the enable field is "enabled" (0b110);
 *  - the charger: enable bit (CNFG_CHG_B), charge voltage 3.6 V + 25 mV * code (CNFG_CHG_G), fast charge current
 *    7.5 mA * (code + 1) (CNFG_CHG_E);
 *  - the charger and input status the test writes to STAT_CHG_B.
 * Not modelled: the interrupt masks, the nEN and nIRQ pins (the test drives the GPIO emulator), the watchdog,
 * flexible power sequencer slots (their enable codes read as off), the AMUX analog path (the ADC emulator answers). */

#define DT_DRV_COMPAT sensei_test_max77654

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "emul_max77654.h"
#include "max77654_reg.h"

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one PMIC node");

#define REG_INT_GLBL0 MAX77654__REG_INT_GLBL0
#define REG_INT_CHG MAX77654__REG_INT_CHG
#define REG_STAT_CHG_A MAX77654__REG_STAT_CHG_A
#define REG_STAT_CHG_B MAX77654__REG_STAT_CHG_B
#define REG_INT_GLBL1 MAX77654__REG_INT_GLBL1
#define REG_CID MAX77654__REG_CID

static struct pmic_emul st;
static uint8_t regs[PMIC_EMUL_REGS];
static uint8_t pend_glbl0, pend_glbl1, pend_chg;
static int64_t busy_until_ms;
static bool first_done;

struct pmic_emul *pmic_emul_get(void) { return &st; }

void pmic_emul_por(void) {
  memset(regs, 0, sizeof(regs));
  /* the enable fields come up "disabled" */
  regs[MAX77654__REG_CNFG_SBB0_B] = regs[MAX77654__REG_CNFG_SBB1_B] = regs[MAX77654__REG_CNFG_SBB2_B] =
      MAX77654__REG_CNFG_SBB0_B__FIELD_EN_SBB__CONST_DISABLED;
  regs[MAX77654__REG_CNFG_LDO0_B] = regs[MAX77654__REG_CNFG_LDO1_B] =
      MAX77654__REG_CNFG_LDO0_B__FIELD_EN_LDO__CONST_DISABLED;
  pend_glbl0 = pend_glbl1 = pend_chg = 0;
}

void pmic_emul_reset_test_state(void) {
  memset(&st, 0, sizeof(st));
  st.present = true;
  st.cid = 0x02;
  st.fail_read_reg = -1;
  st.fail_write_reg = -1;
  busy_until_ms = 0;
  first_done = false;
}

uint8_t pmic_emul_reg(uint8_t reg) { return regs[reg]; }
void pmic_emul_set_reg(uint8_t reg, uint8_t val) { regs[reg] = val; }

void pmic_emul_raise(uint8_t glbl0, uint8_t glbl1, uint8_t chg) {
  pend_glbl0 |= glbl0;
  pend_glbl1 |= glbl1;
  pend_chg |= chg;
}

bool pmic_emul_pending(void) { return (pend_glbl0 | pend_glbl1 | pend_chg) != 0; }

void pmic_emul_set_charger(uint8_t chg_dtls, uint8_t chgin_dtls, bool charging) {
  regs[REG_STAT_CHG_B] = (uint8_t)((chg_dtls << 4) | (chgin_dtls << 2) | (charging ? 0x02 : 0));
}

int pmic_emul_sbb_mv(int sbb) {
  uint8_t a = (uint8_t)(MAX77654__REG_CNFG_SBB0_A + 2 * sbb); /* SBB0 29/2A, SBB1 2B/2C, SBB2 2D/2E */
  uint8_t b = a + 1;

  if ((regs[b] & MAX77654__REG_CNFG_SBB0_B__FIELD_EN_SBB) != MAX77654__REG_CNFG_SBB0_B__FIELD_EN_SBB__CONST_ENABLED) {
    return 0;
  }
  return 800 + 50 * (regs[a] & MAX77654__REG_CNFG_SBB0_A__FIELD_TV_SBB);
}

int pmic_emul_ldo_mv(int ldo) {
  uint8_t a = (uint8_t)(MAX77654__REG_CNFG_LDO0_A + 2 * ldo);
  uint8_t b = a + 1;

  if ((regs[b] & MAX77654__REG_CNFG_LDO0_B__FIELD_EN_LDO) != MAX77654__REG_CNFG_LDO0_B__FIELD_EN_LDO__CONST_ENABLED) {
    return 0;
  }
  return 800 + 25 * (regs[a] & MAX77654__REG_CNFG_LDO0_A__FIELD_TV_LDO);
}

bool pmic_emul_charger_enabled(void) {
  return (regs[MAX77654__REG_CNFG_CHG_B] & MAX77654__REG_CNFG_CHG_B__FIELD_CHG_EN) != 0;
}

int pmic_emul_charge_cv_mv(void) { return 3600 + 25 * (regs[MAX77654__REG_CNFG_CHG_G] >> 2); }

int pmic_emul_charge_cc_x10ma(void) { return 75 * ((regs[MAX77654__REG_CNFG_CHG_E] >> 2) + 1); }

int pmic_emul_amux(void) { return regs[MAX77654__REG_CNFG_CHG_I] & MAX77654__REG_CNFG_CHG_I__FIELD_MUX_SEL; }

static int nack(void) {
  st.nacks++;
  return -EIO;
}

static uint8_t read_reg(uint8_t reg) {
  uint8_t v;

  switch (reg) {
  case REG_INT_GLBL0:
    v = pend_glbl0;
    pend_glbl0 = 0;
    return v;
  case REG_INT_GLBL1:
    v = pend_glbl1;
    pend_glbl1 = 0;
    return v;
  case REG_INT_CHG:
    v = pend_chg;
    pend_chg = 0;
    return v;
  case REG_CID:
    return st.cid;
  default:
    return reg < PMIC_EMUL_REGS ? regs[reg] : 0;
  }
}

static void write_reg(uint8_t reg, uint8_t val) {
  int64_t now = k_uptime_get();

  if (reg >= PMIC_EMUL_REGS) {
    return;
  }
  st.last_write_ms = now;
  st.wcount[reg]++;
  if (st.wlog_len < PMIC_EMUL_WLOG_LEN) {
    st.wlog[st.wlog_len++] = (struct pmic_wlog){.t_ms = now, .reg = reg, .val = val};
  }
  if (reg == MAX77654__REG_CNFG_GLBL &&
      (val & MAX77654__REG_CNFG_GLBL__FIELD_SFT_CTRL) == MAX77654__REG_CNFG_GLBL__FIELD_SFT_CTRL__CONST_SFT_CRST) {
    pmic_emul_por(); /* every register back to its power-up value, the rails go off */
    st.resets++;
    st.reset_ms = now;
    busy_until_ms = now + PMIC_EMUL_RESET_BUSY_MS;
    return;
  }
  regs[reg] = val;
}

static int pmic_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr) {
  uint8_t ptr = 0;
  bool have_ptr = false; /* i2c_burst_write() sends the register in a message of its own */
  int64_t now = k_uptime_get();

  ARG_UNUSED(target);
  st.transfers++;
  if (!first_done) {
    first_done = true;
    if (st.first_access_cb != NULL) {
      st.first_access_cb();
    }
  }
  if (!st.present || addr != DT_INST_REG_ADDR(0)) {
    return nack();
  }
  if (now < busy_until_ms) {
    st.early++;
    return nack();
  }
  for (int i = 0; i < num_msgs; i++) {
    if (msgs[i].flags & I2C_MSG_READ) {
      for (uint32_t n = 0; n < msgs[i].len; n++, ptr++) {
        if (st.fail_read_reg == ptr) {
          return nack();
        }
        msgs[i].buf[n] = read_reg(ptr);
      }
    } else {
      for (uint32_t n = 0; n < msgs[i].len; n++) {
        if (!have_ptr) {
          ptr = msgs[i].buf[n]; /* register pointer */
          have_ptr = true;
          continue;
        }
        if (st.fail_write_reg == ptr) {
          return nack();
        }
        write_reg(ptr++, msgs[i].buf[n]);
      }
    }
  }
  return 0;
}

static const struct i2c_emul_api pmic_emul_api = {
    .transfer = pmic_emul_transfer,
};

static int pmic_emul_init(const struct emul *target, const struct device *parent) {
  ARG_UNUSED(target);
  ARG_UNUSED(parent);
  pmic_emul_reset_test_state();
  pmic_emul_por();
  return 0;
}

/* The emulator needs a device for its node, there is no driver. */
static int pmic_dev_init(const struct device *dev) {
  ARG_UNUSED(dev);
  return 0;
}

#define PMIC_EMUL(n)                                                                                                   \
  DEVICE_DT_INST_DEFINE(n, pmic_dev_init, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);    \
  EMUL_DT_INST_DEFINE(n, pmic_emul_init, NULL, NULL, &pmic_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(PMIC_EMUL)
