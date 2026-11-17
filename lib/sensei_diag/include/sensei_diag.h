/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_diag.h
 * @brief Field diagnostics, pure part: reset cause bits, fault record and boot state in their byte form. Plain C, no
 * Zephyr, so the firmware (NRF/system/diag), the host tests and the host tools use the same layout.
 */

#ifndef SENSEI_DIAG_H
#define SENSEI_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Reset cause ---- */

/**
 * Reset cause bit field (Status.reset_cause). Bits 0 to 16 have the positions of the RESET_* flags of the Zephyr
 * hwinfo API (NRF/system/diag checks that at build time), so a reset that hwinfo reports maps one to one. Bits
 * 24 and up are added by sensei_diag_reset_map().
 */
#define SENSEI_RESET_PIN (1u << 0)
#define SENSEI_RESET_SOFTWARE (1u << 1)
#define SENSEI_RESET_BROWNOUT (1u << 2)
#define SENSEI_RESET_POR (1u << 3)
#define SENSEI_RESET_WATCHDOG (1u << 4)
#define SENSEI_RESET_DEBUG (1u << 5)
#define SENSEI_RESET_SECURITY (1u << 6)
#define SENSEI_RESET_LOW_POWER_WAKE (1u << 7)
#define SENSEI_RESET_CPU_LOCKUP (1u << 8)
#define SENSEI_RESET_PARITY (1u << 9)
#define SENSEI_RESET_PLL (1u << 10)
#define SENSEI_RESET_CLOCK (1u << 11)
#define SENSEI_RESET_HARDWARE (1u << 12)
#define SENSEI_RESET_USER (1u << 13)
#define SENSEI_RESET_TEMPERATURE (1u << 14)
#define SENSEI_RESET_BOOTLOADER (1u << 15)
#define SENSEI_RESET_FLASH (1u << 16)
/** The reset followed a fatal error that the fault handler recorded (the record is valid). */
#define SENSEI_RESET_FAULT (1u << 24)
/** hwinfo is not supported on this platform: the cause is not known. */
#define SENSEI_RESET_UNKNOWN (1u << 25)

/** Bits that hwinfo reports (0 to 16). */
#define SENSEI_RESET_HWINFO_MASK 0x1FFFFu

/**
 * @brief Reset cause of this boot from the hwinfo flags.
 *
 * The nRF reset reason register is empty after a power-on reset and after a brown-out, and hwinfo then reports no
 * flag: this is mapped to POR (a brown-out cannot be told apart). hwinfo not supported (hw_ok false) gives UNKNOWN.
 * Bits above the hwinfo range are dropped. A valid fault record adds FAULT.
 *
 * @param hwinfo_flags RESET_* flags from hwinfo_get_reset_cause().
 * @param hw_ok        True if hwinfo_get_reset_cause() worked.
 * @param fault_valid  True if the retained RAM held a valid fault record at boot.
 */
uint32_t sensei_diag_reset_map(uint32_t hwinfo_flags, bool hw_ok, bool fault_valid);

/** @brief Name of one reset cause bit (0 to 31), "" for a bit without a name. */
const char *sensei_diag_reset_name(unsigned bit);

/**
 * @brief Text of a cause: the names of the set bits joined with '|' ("-" for none). Cut to fit and always
 * terminated. Returns the length that the full text needs, as snprintf does.
 */
size_t sensei_diag_reset_format(uint32_t cause, char *buf, size_t size);

/* ---- Fault record ---- */

/**
 * Reason codes: the K_ERR_* of Zephyr (0 to 4; from 16 the ones of the architecture, K_ERR_ARM_* for Cortex-M, as in
 * Zephyr 4.4) and the codes of SENSEI_FAULT_*. Zephyr clears the fault status registers (CFSR) before it calls the
 * fatal handler, so the reason is what says which fault it was.
 */
#define SENSEI_FAULT_CPU_EXCEPTION 0
#define SENSEI_FAULT_SPURIOUS_IRQ 1
#define SENSEI_FAULT_STACK_CHK_FAIL 2
#define SENSEI_FAULT_KERNEL_OOPS 3
#define SENSEI_FAULT_KERNEL_PANIC 4
/** Fault of the nRF91 modem core (reported by the modem library, not by the application core). */
#define SENSEI_FAULT_MODEM 0x80
#define SENSEI_FAULT_UNKNOWN 0xFF

/** Fault record flag: a coredump of the faulting thread was kept (NRF/system/diag, CONFIG_SENSEI_DIAG_COREDUMP). */
#define SENSEI_FAULT_FLAG_COREDUMP 0x01

/** Characters of the thread name in a record, with the terminating NUL. */
#define SENSEI_DIAG_NAME_LEN 16

struct sensei_diag_fault {
  uint8_t reason;    /**< SENSEI_FAULT_* */
  uint8_t flags;     /**< SENSEI_FAULT_FLAG_* */
  uint16_t last_job; /**< job id the application had set last (0: none) */
  uint32_t pc;       /**< program counter of the fault (0: not known) */
  uint32_t lr;       /**< link register of the fault (0: not known) */
  uint32_t status;   /**< Cortex-M: stacked xPSR (bits 0 to 8: interrupt that the fault hit, 0 in a thread), else 0 */
  uint32_t uptime_s; /**< uptime at the fault, s */
  uint32_t image_id; /**< id of the running image (the application sets it, 0: not set): picks the ELF for addr2line */
  uint32_t boot_count;               /**< boot counter of the boot that faulted */
  char thread[SENSEI_DIAG_NAME_LEN]; /**< name of the faulting thread, "ISR" in an interrupt */
};

/** Bytes of a packed fault record. */
#define SENSEI_DIAG_FAULT_BYTES 52

/**
 * @brief Pack a record (little endian, with a magic, a version and a CRC-32) into out[SENSEI_DIAG_FAULT_BYTES].
 * The thread name is cut and filtered to printable ASCII.
 */
void sensei_diag_fault_pack(const struct sensei_diag_fault *f, uint8_t *out);

/**
 * @brief Unpack and check a record.
 * @return 0, -EINVAL (wrong size or NULL), -ENOMSG (no magic or another version: not a record), -EBADMSG (CRC
 * mismatch: damaged).
 */
int sensei_diag_fault_unpack(const uint8_t *in, size_t len, struct sensei_diag_fault *f);

/** @brief Name of a reason code ("exception", "stack", "usage-undefined-instruction", ...), "unknown" if there is none.
 */
const char *sensei_diag_fault_reason_name(uint8_t reason);

/* ---- Boot state ---- */

/** Bytes of the packed boot state. */
#define SENSEI_DIAG_STATE_BYTES 12

/**
 * @brief Count a boot in the packed state of the retained RAM. A valid state gives count + 1, else (RAM lost
 * at power-up, first boot) the count restarts at 1. The new state is written back into state[].
 *
 * @param state     SENSEI_DIAG_STATE_BYTES bytes read from the retained RAM; the new state on return.
 * @param had_state Set to true if the old state was valid (NULL: not wanted).
 * @return The boot count of this boot (saturates at UINT32_MAX).
 */
uint32_t sensei_diag_boot_count_next(uint8_t *state, bool *had_state);

/** @brief CRC-32 (IEEE 802.3, as in Zephyr crc32_ieee) of a buffer. */
uint32_t sensei_diag_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DIAG_H */
