/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_diag.h"

#include <errno.h>
#include <string.h>

#define FAULT_MAGIC 0xD1F7u
#define FAULT_VERSION 1u
#define STATE_MAGIC 0xD1B0u
#define STATE_VERSION 1u

static const char *const reset_names[] = {
    [0] = "PIN",         [1] = "SOFT",     [2] = "BROWNOUT",  [3] = "POR",      [4] = "WDT",
    [5] = "DEBUG",       [6] = "SECURITY", [7] = "WAKE",      [8] = "LOCKUP",   [9] = "PARITY",
    [10] = "PLL",        [11] = "CLOCK",   [12] = "HARDWARE", [13] = "USER",    [14] = "TEMP",
    [15] = "BOOTLOADER", [16] = "FLASH",   [24] = "FAULT",    [25] = "UNKNOWN",
};

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }

uint32_t sensei_diag_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;

  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

/* ---- Reset cause ---- */

uint32_t sensei_diag_reset_map(uint32_t hwinfo_flags, bool hw_ok, bool fault_valid) {
  uint32_t cause = 0;

  if (!hw_ok) {
    cause = SENSEI_RESET_UNKNOWN;
  } else {
    cause = hwinfo_flags & SENSEI_RESET_HWINFO_MASK;
    if (cause == 0) {
      cause = SENSEI_RESET_POR;
    }
  }
  /* A lockup (a fault inside a fault handler, for example an overflow of the interrupt stack) leaves no record, but it
   * is a fatal error all the same: it counts like one. */
  if (fault_valid || (cause & SENSEI_RESET_CPU_LOCKUP) != 0) {
    cause |= SENSEI_RESET_FAULT;
  }
  return cause;
}

const char *sensei_diag_reset_name(unsigned bit) {
  if (bit < sizeof(reset_names) / sizeof(reset_names[0]) && reset_names[bit] != NULL) {
    return reset_names[bit];
  }
  return "";
}

static void put_char(char *buf, size_t size, size_t *n, char c) {
  if (buf != NULL && *n + 1 < size) {
    buf[*n] = c;
  }
  (*n)++;
}

size_t sensei_diag_reset_format(uint32_t cause, char *buf, size_t size) {
  size_t n = 0;

  for (unsigned bit = 0; bit < 32; bit++) {
    const char *name = sensei_diag_reset_name(bit);

    if ((cause & (1u << bit)) == 0 || name[0] == '\0') {
      continue;
    }
    if (n != 0) {
      put_char(buf, size, &n, '|');
    }
    for (; *name != '\0'; name++) {
      put_char(buf, size, &n, *name);
    }
  }
  if (n == 0) {
    put_char(buf, size, &n, '-');
  }
  if (buf != NULL && size > 0) {
    buf[n < size ? n : size - 1] = '\0';
  }
  return n;
}

/* ---- Fault record ----
 * Layout (little endian): 0 magic u16, 2 version u8, 3 reason u8, 4 pc, 8 lr, 12 status, 16 uptime_s,
 * 20 image_id, 24 boot_count (u32 each), 28 last_job u16, 30 flags u16, 32 thread[16], 48 CRC-32 of 0 to 47. */

void sensei_diag_fault_pack(const struct sensei_diag_fault *f, uint8_t *out) {
  memset(out, 0, SENSEI_DIAG_FAULT_BYTES);
  put16(out, FAULT_MAGIC);
  out[2] = FAULT_VERSION;
  out[3] = f->reason;
  put32(out + 4, f->pc);
  put32(out + 8, f->lr);
  put32(out + 12, f->status);
  put32(out + 16, f->uptime_s);
  put32(out + 20, f->image_id);
  put32(out + 24, f->boot_count);
  put16(out + 28, f->last_job);
  put16(out + 30, f->flags);
  for (size_t i = 0; i + 1 < SENSEI_DIAG_NAME_LEN && f->thread[i] != '\0'; i++) {
    char c = f->thread[i];

    out[32 + i] = (c >= 0x20 && c < 0x7F) ? (uint8_t)c : (uint8_t)'?';
  }
  put32(out + 48, sensei_diag_crc32(out, 48));
}

int sensei_diag_fault_unpack(const uint8_t *in, size_t len, struct sensei_diag_fault *f) {
  if (in == NULL || f == NULL || len != SENSEI_DIAG_FAULT_BYTES) {
    return -EINVAL;
  }
  if (get16(in) != FAULT_MAGIC || in[2] != FAULT_VERSION) {
    return -ENOMSG;
  }
  if (get32(in + 48) != sensei_diag_crc32(in, 48)) {
    return -EBADMSG;
  }
  memset(f, 0, sizeof(*f));
  f->reason = in[3];
  f->pc = get32(in + 4);
  f->lr = get32(in + 8);
  f->status = get32(in + 12);
  f->uptime_s = get32(in + 16);
  f->image_id = get32(in + 20);
  f->boot_count = get32(in + 24);
  f->last_job = get16(in + 28);
  f->flags = (uint8_t)get16(in + 30);
  memcpy(f->thread, in + 32, SENSEI_DIAG_NAME_LEN - 1);
  f->thread[SENSEI_DIAG_NAME_LEN - 1] = '\0';
  return 0;
}

/* K_ERR_ARM_* of Zephyr 4.4 (arch/arm/arch.h), Cortex-M: from 16. */
static const char *const arm_reasons[] = {
    "mem-generic",
    "mem-stacking",
    "mem-unstacking",
    "mem-data-access",
    "mem-instruction-access",
    "mem-fp-lazy",
    "bus-generic",
    "bus-stacking",
    "bus-unstacking",
    "bus-precise-data",
    "bus-imprecise-data",
    "bus-instruction",
    "bus-fp-lazy",
    "usage-generic",
    "usage-div-0",
    "usage-unaligned",
    "usage-stack-overflow",
    "usage-no-coprocessor",
    "usage-illegal-exc-return",
    "usage-illegal-epsr",
    "usage-undefined-instruction",
    "secure-generic",
    "secure-entry-point",
    "secure-integrity-signature",
    "secure-exception-return",
    "secure-attribution-unit",
    "secure-transition",
    "secure-fp-lazy",
    "secure-lazy-state-error",
};

const char *sensei_diag_fault_reason_name(uint8_t reason) {
  if (reason >= 16 && reason - 16u < sizeof(arm_reasons) / sizeof(arm_reasons[0])) {
    return arm_reasons[reason - 16u];
  }
  switch (reason) {
  case SENSEI_FAULT_CPU_EXCEPTION:
    return "exception";
  case SENSEI_FAULT_SPURIOUS_IRQ:
    return "spurious-irq";
  case SENSEI_FAULT_STACK_CHK_FAIL:
    return "stack";
  case SENSEI_FAULT_KERNEL_OOPS:
    return "oops";
  case SENSEI_FAULT_KERNEL_PANIC:
    return "panic";
  case SENSEI_FAULT_MODEM:
    return "modem";
  default:
    return reason >= 16 && reason < 0x80 ? "arch" : "unknown";
  }
}

/* ---- Boot state ----
 * 0 magic u16, 2 version u8, 3 reserved, 4 boot_count u32, 8 CRC-32 of 0 to 7. */

uint32_t sensei_diag_boot_count_next(uint8_t *state, bool *had_state) {
  uint32_t count = 0;
  bool valid =
      get16(state) == STATE_MAGIC && state[2] == STATE_VERSION && get32(state + 8) == sensei_diag_crc32(state, 8);

  if (valid) {
    count = get32(state + 4);
  }
  if (count != UINT32_MAX) {
    count++;
  }
  memset(state, 0, SENSEI_DIAG_STATE_BYTES);
  put16(state, STATE_MAGIC);
  state[2] = STATE_VERSION;
  put32(state + 4, count);
  put32(state + 8, sensei_diag_crc32(state, 8));
  if (had_state != NULL) {
    *had_state = valid;
  }
  return count;
}
