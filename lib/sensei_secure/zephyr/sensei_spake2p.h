/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The point arithmetic of the commissioning (SPAKE2+ on P-256) behind the vtable of sensei_secure_zephyr.c. One of two
 * files defines these (CONFIG_SENSEI_SECURE_SPAKE2P_OCRYPTO or _MBEDTLS_ECP). The contract of each function is the one
 * of the vtable entry of the same name in sensei_secure.h. */

#ifndef SENSEI_SPAKE2P_H_
#define SENSEI_SPAKE2P_H_

#include <stdint.h>

int sensei_spake2p_reduce(const uint8_t in[40], uint8_t out[32]);
int sensei_spake2p_check(const uint8_t pt[65]);
int sensei_spake2p_share(const uint8_t w0[32], const uint8_t xy[32], const uint8_t mn[65], uint8_t share[65]);
int sensei_spake2p_zv(const uint8_t w0[32], const uint8_t *w1, const uint8_t xy[32], const uint8_t peer[65],
                      const uint8_t nm[65], const uint8_t *l, uint8_t z[65], uint8_t v[65]);

#endif /* SENSEI_SPAKE2P_H_ */
