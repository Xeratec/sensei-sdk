/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_diag_rt.h
 * @brief Field diagnostics on Zephyr: reset cause, boot counter and the record of the last fatal error.
 * The types and the byte layout are in sensei_diag.h (lib/sensei_diag).
 */

#ifndef SENSEI_DIAG_RT_H_
#define SENSEI_DIAG_RT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fatal.h>
#include <zephyr/sys/util.h>

#include "sensei_diag.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read the reset cause, count the boot and take over the record of the last fault from the retained RAM. Runs by
 * itself at boot (SYS_INIT, POST_KERNEL), before the application. Calling it again (tests) counts another boot.
 * The record is moved out of the retained RAM here, so it exists once: later resets do not bring it back.
 * It also clears the hwinfo reset cause: use sensei_diag_reset_cause() instead of reading hwinfo.
 */
int sensei_diag_boot(void);

/** Reset cause of this boot, SENSEI_RESET_* bits (sensei_diag.h). */
uint32_t sensei_diag_reset_cause(void);

/** Raw hwinfo flags of this boot (RESET_*), 0 if hwinfo is not supported. */
uint32_t sensei_diag_reset_cause_hw(void);

/** Boot counter: boots since the retained RAM was last lost (power-up, brown-out). 1 on the first boot. */
uint32_t sensei_diag_boot_count(void);

/** True if a retention area keeps the record (devicetree), false if it is in noinit RAM. */
bool sensei_diag_retained(void);

/**
 * The record of the fatal error that ended the previous boot: copy it out and forget it (read and clear).
 * Returns false if there was none, or it was taken already. Persisting it is the application's choice (settings:
 * store the bytes of sensei_diag_fault_pack()).
 */
bool sensei_diag_take_fault(struct sensei_diag_fault *out);

/** The same, without clearing. */
bool sensei_diag_peek_fault(struct sensei_diag_fault *out);

/** Job id that goes into the next fault record (0: none). Cheap: one store. Use small stable numbers. */
void sensei_diag_set_job(uint16_t id);

/** Image id that goes into the next fault record, for example the first 4 bytes of the image hash. */
void sensei_diag_set_image_id(uint32_t id);

/**
 * Write a fault record into the retained RAM with the values the caller knows; the uptime, boot count, job id and image
 * id are added. For faults that Zephyr does not see, such as the modem core of the nRF91.
 * @param reason SENSEI_FAULT_*
 * @param thread Name that goes into the record (NULL: "?").
 */
void sensei_diag_capture_ext(uint8_t reason, uint32_t pc, uint32_t lr, uint32_t status, const char *thread);

/**
 * Write the fault record of this boot into the retained RAM. Called by the fatal handler
 * (CONFIG_SENSEI_DIAG_FATAL_HANDLER); an application with its own handler calls it first and then resets. Safe in
 * interrupt and fault context.
 *
 * @param reason One of the K_ERR_* of Zephyr (or SENSEI_FAULT_*).
 * @param esf    The exception frame, may be NULL (PC and LR are then 0).
 */
void sensei_diag_capture(unsigned int reason, const struct arch_esf *esf);

#ifdef CONFIG_SENSEI_DIAG_COREDUMP
/**
 * Copy the kept coredump of the previous boot (only after a fault whose record has SENSEI_FAULT_FLAG_COREDUMP).
 * @return Bytes copied (at most max), 0 if there is none.
 */
size_t sensei_diag_dump_read(size_t offset, uint8_t *buf, size_t max);

/** Size of the kept coredump in bytes, 0 if there is none. */
size_t sensei_diag_dump_size(void);

/** Forget the kept coredump. */
void sensei_diag_dump_clear(void);

/* Used by the fault capture: true if the coredump of this fault is complete in the retained RAM. */
bool sensei_diag_dump_valid(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DIAG_RT_H_ */
