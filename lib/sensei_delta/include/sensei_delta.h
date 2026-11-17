/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Delta coder for windows of seven sensor channels: closed-loop DPCM with a fixed quantizer step, then static
 * rANS or canonical Huffman coding. The engine is generic: the trained code tables, the scaler and the
 * quantizer steps come in a struct sensei_delta_tables, made by the application.
 */

#ifndef SENSEI_DELTA_H
#define SENSEI_DELTA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format version 1: seven channels, 130 symbols (q = -64 ... 64 and the escape). */
#define SENSEI_DELTA_FORMAT_VERSION 1
#define SENSEI_DELTA_N_CH 7
#define SENSEI_DELTA_N_SYM 130
#define SENSEI_DELTA_ESC 129

/* Entropy coder ids (cfg.entropy). Either one can be compiled out with the macros below. */
#define SENSEI_DELTA_ENTROPY_RANS 0
#define SENSEI_DELTA_ENTROPY_HUFFMAN 1

#ifndef SENSEI_DELTA_ENABLE_RANS
#define SENSEI_DELTA_ENABLE_RANS 1
#endif
#ifndef SENSEI_DELTA_ENABLE_HUFFMAN
#define SENSEI_DELTA_ENABLE_HUFFMAN 1
#endif

/* Window length limits and sizes. u is [7][n]: u[c * n + t]. */
#define SENSEI_DELTA_MAX_N 512
#define SENSEI_DELTA_HEADER_BYTES 14 /* u[0] of the seven channels */
/* Symbol scratch for the encoder: one byte per coded symbol. */
#define SENSEI_DELTA_SCRATCH_SIZE(n) (7u * ((unsigned)(n) - 1u))
/* Worst case payload: every symbol an escape (rANS: 4 B, Huffman: under 4 B) plus final state. */
#define SENSEI_DELTA_MAX_PAYLOAD(n) (SENSEI_DELTA_HEADER_BYTES + 4u + 28u * ((unsigned)(n) - 1u))

/* Return codes. */
#define SENSEI_DELTA_OK 0
#define SENSEI_DELTA_E_CFG (-1)     /* bad set, level or entropy id, or coder compiled out */
#define SENSEI_DELTA_E_NOSET (-2)   /* table set compiled out */
#define SENSEI_DELTA_E_SIZE (-3)    /* output buffer too small */
#define SENSEI_DELTA_E_N (-4)       /* n out of range */
#define SENSEI_DELTA_E_SCRATCH (-5) /* scratch buffer too small */
#define SENSEI_DELTA_E_CORRUPT (-6) /* decoder: invalid or inconsistent payload */
#define SENSEI_DELTA_E_ARG (-7)     /* NULL pointer */

/*
 * One table set: symbol frequencies (15 bit, each at least 1, sum 32768) and Huffman code lengths (1 ... 15,
 * Kraft sum 1) for every level and channel. A set can be NULL (not built); len can be NULL without Huffman.
 */
struct sensei_delta_set {
  const uint16_t (*freq)[SENSEI_DELTA_N_CH][SENSEI_DELTA_N_SYM]; /* [level][ch][sym] */
  const uint8_t (*len)[SENSEI_DELTA_N_CH][SENSEI_DELTA_N_SYM];   /* [level][ch][sym] */
};

/* Everything the engine needs from the application's training. All pointers must stay valid. */
struct sensei_delta_tables {
  uint8_t n_sets;
  uint8_t n_levels;
  const uint16_t *level_step;          /* [n_levels] quantizer step in codec units */
  const double *lo;                    /* [7] scaler offset */
  const double *k;                     /* [7] scaler gain, 32768 / (max - min) */
  const int32_t *prefilter_a;          /* [7] prefilter coefficients, Q15 */
  const struct sensei_delta_set *sets; /* [n_sets] */
};

struct sensei_delta_cfg {
  uint8_t set;     /* index into tables->sets */
  uint8_t level;   /* index into tables->level_step */
  uint8_t entropy; /* SENSEI_DELTA_ENTROPY_* */
};

/* Codec units: u = clamp(floor((x - lo) * k + 0.5) + 16384, 0, 65535), plain IEEE double operations. */
uint16_t sensei_delta_to_units(const struct sensei_delta_tables *tab, unsigned ch, double x);
double sensei_delta_from_units(const struct sensei_delta_tables *tab, unsigned ch, int32_t u);

/* Prefilter: first-order low-pass per channel, state runs on across windows. */
struct sensei_delta_prefilter {
  int32_t y[SENSEI_DELTA_N_CH];
  uint8_t primed;
};
void sensei_delta_prefilter_reset(struct sensei_delta_prefilter *f);
/* One sample of all seven channels. in and out may alias. */
void sensei_delta_prefilter_step(const struct sensei_delta_tables *tab, struct sensei_delta_prefilter *f,
                                 const uint16_t in[7], uint16_t out[7]);

/*
 * Encode one window of n samples (1 <= n <= 512). u is [7][n] codec units.
 * scratch needs SENSEI_DELTA_SCRATCH_SIZE(n) bytes. out needs up to SENSEI_DELTA_MAX_PAYLOAD(n)
 * bytes; a smaller buffer works if the actual payload fits. Returns 0 or a negative code.
 */
int sensei_delta_encode(const struct sensei_delta_tables *tab, const struct sensei_delta_cfg *cfg, const uint16_t *u,
                        uint16_t n, uint8_t *out, size_t out_cap, size_t *out_len, uint8_t *scratch,
                        size_t scratch_len);

/* Decode into uhat [7][n] (int32, not clamped). Checks that the payload is consumed exactly. */
int sensei_delta_decode(const struct sensei_delta_tables *tab, const struct sensei_delta_cfg *cfg, const uint8_t *in,
                        size_t in_len, uint16_t n, int32_t *uhat);

/*
 * Profiling hooks. Default: nothing. Define SENSEI_DELTA_PORT_NRF5340, SENSEI_DELTA_PORT_GAP9 or
 * SENSEI_DELTA_PORT_HOST to use sensei_delta_port.h, or define the macros yourself. BEGIN and END
 * must be in the same scope. Tags: dpcm, coder.
 */
#if defined(SENSEI_DELTA_PORT_NRF5340) || defined(SENSEI_DELTA_PORT_GAP9) || defined(SENSEI_DELTA_PORT_HOST)
#include <sensei_delta_port.h>
#endif
#ifndef SENSEI_DELTA_PROF_BEGIN
#define SENSEI_DELTA_PROF_BEGIN(tag)
#define SENSEI_DELTA_PROF_END(tag)
#endif

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DELTA_H */
