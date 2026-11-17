/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_upd.h
 * @brief What the nRF5340 client remembers about the update of the shield, as pure functions (time is an argument,
 *        no Zephyr): when a swap may be running, how long to wait for the shield, who owns the trial of a new image,
 *        when a session ends. lpwan_client.c calls them; the host tests check the rules.
 *
 * - Swap. MCUboot swaps the images of the shield at a power-up (25 to 45 s) after an apply, after a restart that
 *   follows a trial image (it reverts), and when the node reset while a swap ran. During a swap the shield does not
 *   answer, and a power cut in the middle starts it again. So the client waits LPWAN_UPD_SWAP_TIMEOUT_MS, without
 *   cutting the power, whenever a swap may be running: at the first bring-up after the node started, after an apply
 *   was sent, after a power cut while an image was on trial, and after a bring-up that timed out (the next attempt
 *   waits the long time once).
 * - Trial. The client owns the trial of a new shield image: while the version says `trial` it sends dfu_confirm (with
 *   the image id) with every request and keeps the power until `trial` is false, at most LPWAN_UPD_TRIAL_HOLD_MS (the
 *   image reverts after its own timeout). It confirms only an image that has the update support (dfu_targets) that
 *   the running one had.
 * - Session. It ends after LPWAN_UPD_SESSION_IDLE_MS without a request, or LPWAN_UPD_SESSION_MAX_MS after it began
 *   (a sender that renews it for ever holds the uplinks and GAP9), and then LPWAN_UPD_COOLDOWN_MS pass before a new
 * one. While the modem erases or updates, the client asks the shield itself every LPWAN_UPD_POLL_MS: that counts as a
 * request.
 */

#ifndef SENSEI_LPWAN_UPD_H_
#define SENSEI_LPWAN_UPD_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest a swap takes at a power-up (112 sectors of the shield: not measured on hardware yet). */
#define LPWAN_UPD_SWAP_TIMEOUT_MS 180000u
#define LPWAN_UPD_SESSION_IDLE_MS 120000u
#define LPWAN_UPD_SESSION_MAX_MS (45u * 60u * 1000u)
/** After a session that ended for being too long the client takes no new transfer for this long (uplinks and GAP9 get
 *  their turn); status, confirm and abort still go through. */
#define LPWAN_UPD_COOLDOWN_MS (10u * 60u * 1000u)
/** Lease of BUS_DFU after a request; it is renewed while the client waits for a swap. */
#define LPWAN_UPD_BUS_HOLD_MS (LPWAN_UPD_SESSION_IDLE_MS + 30000u)
#define LPWAN_UPD_POLL_MS 5000u
/** The new image resets itself after 180 s without confirm (CONFIG_LPWAN_SHIELD_TRIAL_TIMEOUT_S): hold a bit longer. */
#define LPWAN_UPD_TRIAL_HOLD_MS 200000u

struct lpwan_upd {
  bool swap_maybe;        /* a swap may be running in the shield */
  bool trial;             /* the last version said: the running image is on trial */
  int64_t trial_since;    /* uptime of that version */
  uint32_t known_targets; /* dfu_targets of the last image that was not on trial */
};

/** The node started: it cut the shield power without knowing what the shield was doing, so a swap may be running. */
void lpwan_upd_init(struct lpwan_upd *u);

/** How long a bring-up waits for the shield: `normal_ms`, or the swap time if a swap may be running. */
uint32_t lpwan_upd_boot_timeout(const struct lpwan_upd *u, uint32_t normal_ms);

/** A bring-up ended. `waited_long`: it waited the swap time. A short attempt that failed makes the next one long. */
void lpwan_upd_bringup_end(struct lpwan_upd *u, bool waited_long, bool ok);

/** dfu_apply of the app was sent (the shield may have marked the image, also if the answer is lost). */
void lpwan_upd_apply_sent(struct lpwan_upd *u);

/** The shield power is cut: an image on trial goes back at the next power-up, which is a swap. */
void lpwan_upd_power_cut(struct lpwan_upd *u);

/** A version arrived (handshake, status). */
void lpwan_upd_version(struct lpwan_upd *u, const sensei_LpwanVersion *v, int64_t now_ms);

/** The image is on trial: send dfu_confirm with every request. */
bool lpwan_upd_confirm_due(const struct lpwan_upd *u);

/** The handshake of the image on trial is good: protocol, update support kept, and a dfu_status round trip worked. */
bool lpwan_upd_may_confirm(const struct lpwan_upd *u, const sensei_LpwanVersion *v, uint32_t protocol, bool status_ok);

/** The shield must stay powered (an image is on trial and has time left to confirm). */
bool lpwan_upd_keep_power(const struct lpwan_upd *u, int64_t now_ms);

enum lpwan_upd_end {
  LPWAN_UPD_CONTINUE = 0,
  LPWAN_UPD_END_IDLE,  /* no request for a while */
  LPWAN_UPD_END_TOTAL, /* too long since it began */
};

/** Does the session go on? start: when it began, last_req: time of the last request (or poll). */
enum lpwan_upd_end lpwan_upd_session_check(int64_t start_ms, int64_t last_req_ms, int64_t now_ms);

/** The pause after a too long session is not over yet. */
bool lpwan_upd_cooling(int64_t until_ms, int64_t now_ms);

/** True if the client should ask the shield itself now: the modem works (erase or update) and no request came. */
bool lpwan_upd_poll_due(bool modem_busy, int64_t last_req_ms, int64_t now_ms);

/** The modem works on its own: DfuStatus of the modem target in the state ERASING or APPLYING. */
bool lpwan_upd_modem_busy(const sensei_DfuStatus *st);

/** After a dfu_apply whose answer was lost: the status shows that the shield marked the app image. */
bool lpwan_upd_apply_took_effect(const sensei_DfuStatus *st);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_UPD_H_ */
