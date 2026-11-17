/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SENSEI delta coder engine, format version 1. The tables come from the caller. */

#include <math.h>
#include <string.h>

#include "sensei_delta.h"

#if defined(SENSEI_DELTA_PORT_NRF5340) || defined(SENSEI_DELTA_PORT_GAP9) || defined(SENSEI_DELTA_PORT_HOST)
uint32_t sensei_delta_prof_cycles[SENSEI_DELTA_PROF_N];
#endif

#define N_CH SENSEI_DELTA_N_CH
#define N_SYM SENSEI_DELTA_N_SYM
#define ESC SENSEI_DELTA_ESC
#define Q_MAX 64
#define HDR SENSEI_DELTA_HEADER_BYTES

/* ---- codec units ---- */

uint16_t sensei_delta_to_units(const struct sensei_delta_tables *tab, unsigned ch, double x) {
  /* Exactly: subtract, multiply, add 0.5, floor. Build with -ffp-contract=off. */
  double v = x - tab->lo[ch];
  v = v * tab->k[ch];
  v = v + 0.5;
  v = floor(v);
  if (!(v > -16384.0)) { /* also catches NaN */
    return 0;
  }
  if (v >= 49151.0) {
    return 65535;
  }
  return (uint16_t)((int32_t)v + 16384);
}

double sensei_delta_from_units(const struct sensei_delta_tables *tab, unsigned ch, int32_t u) {
  return tab->lo[ch] + (double)(u - 16384) / tab->k[ch];
}

/* ---- prefilter ---- */

void sensei_delta_prefilter_reset(struct sensei_delta_prefilter *f) { memset(f, 0, sizeof(*f)); }

void sensei_delta_prefilter_step(const struct sensei_delta_tables *tab, struct sensei_delta_prefilter *f,
                                 const uint16_t in[7], uint16_t out[7]) {
  unsigned c;
  for (c = 0; c < N_CH; c++) {
    int32_t u = (int32_t)in[c];
    int32_t y = f->y[c];
    if (!f->primed) {
      y = u;
    } else {
      int64_t p = (int64_t)tab->prefilter_a[c] * (int64_t)(u - y);
      y += (int32_t)((p + 16384) >> 15); /* arithmetic shift, rounds toward -inf */
    }
    f->y[c] = y;
    out[c] = (uint16_t)y;
  }
  f->primed = 1;
}

/* ---- helpers ---- */

static int check_cfg(const struct sensei_delta_tables *tab, const struct sensei_delta_cfg *cfg,
                     const struct sensei_delta_set **set, uint16_t *step) {
  if (tab == NULL || cfg == NULL) {
    return SENSEI_DELTA_E_ARG;
  }
  if (cfg->set >= tab->n_sets || cfg->level >= tab->n_levels) {
    return SENSEI_DELTA_E_CFG;
  }
  if (cfg->entropy == SENSEI_DELTA_ENTROPY_RANS) {
    if (!SENSEI_DELTA_ENABLE_RANS) {
      return SENSEI_DELTA_E_CFG;
    }
  } else if (cfg->entropy == SENSEI_DELTA_ENTROPY_HUFFMAN) {
    if (!SENSEI_DELTA_ENABLE_HUFFMAN) {
      return SENSEI_DELTA_E_CFG;
    }
  } else {
    return SENSEI_DELTA_E_CFG;
  }
  *set = &tab->sets[cfg->set];
  /* len is NULL when the Huffman tables are not built; rANS does not need it */
  if ((*set)->freq == NULL || (cfg->entropy == SENSEI_DELTA_ENTROPY_HUFFMAN && (*set)->len == NULL)) {
    return SENSEI_DELTA_E_NOSET;
  }
  *step = tab->level_step[cfg->level];
  return SENSEI_DELTA_OK;
}

/* ---- rANS (follows rans_byte.h by Fabian Giesen, public domain) ---- */

#if SENSEI_DELTA_ENABLE_RANS

#define RANS_L (1u << 23)
#define PROB_BITS 15
#define PROB_SCALE (1u << PROB_BITS)

static void build_cum(const uint16_t (*freq)[N_SYM], uint16_t cum[N_CH][N_SYM]) {
  unsigned c, s;
  for (c = 0; c < N_CH; c++) {
    uint32_t acc = 0;
    for (s = 0; s < N_SYM; s++) {
      cum[c][s] = (uint16_t)acc;
      acc += freq[c][s];
    }
  }
}

/* Encoder step. Writes bytes in front of *p; returns 0, or -1 if it would pass lo. */
static inline int rans_put(uint32_t *x, uint8_t **p, const uint8_t *lo, uint32_t start, uint32_t freq) {
  uint32_t xv = *x;
  uint32_t x_max = ((RANS_L >> PROB_BITS) << 8) * freq;
  while (xv >= x_max) {
    if (*p <= lo) {
      return -1;
    }
    *--(*p) = (uint8_t)(xv & 0xFF);
    xv >>= 8;
  }
  *x = ((xv / freq) << PROB_BITS) + (xv % freq) + start;
  return 0;
}

static int rans_encode(const struct sensei_delta_set *set, unsigned level, const uint16_t *u, uint16_t n,
                       const uint8_t *sym, uint8_t *out, size_t out_cap, size_t *out_len) {
  uint16_t cum[N_CH][N_SYM];
  const uint16_t (*freq)[N_SYM] = set->freq[level];
  uint8_t *lo = out + HDR;
  uint8_t *end = out + out_cap;
  uint8_t *p = end;
  uint32_t x = RANS_L;
  size_t nsym = (size_t)n - 1;
  int c;
  size_t t;

  build_cum(freq, cum);
  for (c = N_CH - 1; c >= 0; c--) {
    const uint8_t *sc = sym + (size_t)c * nsym;
    const uint16_t *uc = u + (size_t)c * n;
    for (t = nsym; t >= 1; t--) {
      uint32_t s = sc[t - 1];
      if (s == ESC) {
        uint16_t v = uc[t];
        /* reverse of decode order (sym, hi, lo) */
        if (rans_put(&x, &p, lo, (uint32_t)(v & 0xFF) * 128u, 128u) ||
            rans_put(&x, &p, lo, (uint32_t)(v >> 8) * 128u, 128u)) {
          return SENSEI_DELTA_E_SIZE;
        }
      }
      if (rans_put(&x, &p, lo, cum[c][s], freq[c][s])) {
        return SENSEI_DELTA_E_SIZE;
      }
    }
  }
  if ((size_t)(p - lo) < 4) {
    return SENSEI_DELTA_E_SIZE;
  }
  *--p = (uint8_t)(x >> 24);
  *--p = (uint8_t)(x >> 16);
  *--p = (uint8_t)(x >> 8);
  *--p = (uint8_t)x;
  memmove(lo, p, (size_t)(end - p));
  *out_len = HDR + (size_t)(end - p);
  return SENSEI_DELTA_OK;
}

static int rans_decode(const struct sensei_delta_set *set, unsigned level, uint16_t step, const uint8_t *in,
                       size_t in_len, uint16_t n, int32_t *uhat) {
  uint16_t cum[N_CH][N_SYM];
  const uint16_t (*freq)[N_SYM] = set->freq[level];
  const uint8_t *p = in + HDR;
  const uint8_t *end = in + in_len;
  uint32_t x;
  unsigned c;
  uint16_t t;

  if (in_len < HDR + 4) {
    return SENSEI_DELTA_E_CORRUPT;
  }
  build_cum(freq, cum);
  x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  p += 4;

  for (c = 0; c < N_CH; c++) {
    int32_t *uc = uhat + (size_t)c * n;
    const uint16_t *cc = cum[c];
    for (t = 1; t < n; t++) {
      uint32_t v = x & (PROB_SCALE - 1);
      unsigned lo = 0, hi = N_SYM - 1;
      while (lo < hi) { /* largest s with cum[s] <= v */
        unsigned mid = (lo + hi + 1) >> 1;
        if (cc[mid] <= v) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      x = (uint32_t)freq[c][lo] * (x >> PROB_BITS) + v - cc[lo];
      while (x < RANS_L) {
        if (p >= end) {
          return SENSEI_DELTA_E_CORRUPT;
        }
        x = (x << 8) | *p++;
      }
      if (lo == ESC) {
        uint32_t b[2];
        unsigned i;
        for (i = 0; i < 2; i++) { /* high byte, then low byte */
          v = x & (PROB_SCALE - 1);
          b[i] = v >> 7;
          x = 128u * (x >> PROB_BITS) + v - (b[i] << 7);
          while (x < RANS_L) {
            if (p >= end) {
              return SENSEI_DELTA_E_CORRUPT;
            }
            x = (x << 8) | *p++;
          }
        }
        uc[t] = (int32_t)((b[0] << 8) | b[1]);
      } else {
        uc[t] = uc[t - 1] + ((int32_t)lo - Q_MAX) * (int32_t)step;
      }
    }
  }
  return (x == RANS_L && p == end) ? SENSEI_DELTA_OK : SENSEI_DELTA_E_CORRUPT;
}

#endif /* SENSEI_DELTA_ENABLE_RANS */

/* ---- Huffman (canonical, MSB first) ---- */

#if SENSEI_DELTA_ENABLE_HUFFMAN

#define HUFF_MAXLEN 15

/* Canonical codes for one channel. Returns 0, or -1 on a bad length. */
static int huff_codes(const uint8_t *len, uint16_t code[N_SYM]) {
  uint32_t count[HUFF_MAXLEN + 1] = {0};
  uint32_t next[HUFF_MAXLEN + 1];
  uint32_t c = 0;
  unsigned s, l;

  for (s = 0; s < N_SYM; s++) {
    if (len[s] < 1 || len[s] > HUFF_MAXLEN) {
      return -1;
    }
    count[len[s]]++;
  }
  for (l = 1; l <= HUFF_MAXLEN; l++) {
    c = (c + count[l - 1]) << 1;
    next[l] = c;
  }
  for (s = 0; s < N_SYM; s++) {
    code[s] = (uint16_t)next[len[s]]++;
  }
  return 0;
}

struct bitw {
  uint8_t *p;
  uint8_t *end;
  uint32_t acc;
  unsigned n;
};

static inline int bitw_put(struct bitw *w, uint32_t v, unsigned len) {
  w->acc = (w->acc << len) | v;
  w->n += len;
  while (w->n >= 8) {
    if (w->p >= w->end) {
      return -1;
    }
    w->n -= 8;
    *w->p++ = (uint8_t)(w->acc >> w->n);
  }
  return 0;
}

static int huff_encode(const struct sensei_delta_set *set, unsigned level, const uint16_t *u, uint16_t n,
                       const uint8_t *sym, uint8_t *out, size_t out_cap, size_t *out_len) {
  uint16_t code[N_SYM];
  struct bitw w;
  size_t nsym = (size_t)n - 1;
  unsigned c;
  size_t t;

  w.p = out + HDR;
  w.end = out + out_cap;
  w.acc = 0;
  w.n = 0;
  for (c = 0; c < N_CH; c++) {
    const uint8_t *len = set->len[level][c];
    const uint8_t *sc = sym + (size_t)c * nsym;
    const uint16_t *uc = u + (size_t)c * n;
    if (huff_codes(len, code)) {
      return SENSEI_DELTA_E_CFG;
    }
    for (t = 1; t <= nsym; t++) {
      uint32_t s = sc[t - 1];
      if (bitw_put(&w, code[s], len[s])) {
        return SENSEI_DELTA_E_SIZE;
      }
      if (s == ESC && bitw_put(&w, uc[t], 16)) {
        return SENSEI_DELTA_E_SIZE;
      }
    }
  }
  if (w.n > 0 && bitw_put(&w, 0, 8 - w.n)) { /* pad with zero bits */
    return SENSEI_DELTA_E_SIZE;
  }
  *out_len = (size_t)(w.p - out);
  return SENSEI_DELTA_OK;
}

struct bitr {
  const uint8_t *p;
  const uint8_t *end;
  uint32_t acc; /* left-justified */
  unsigned n;   /* valid bits in acc, zeros are fed past the end */
  size_t used;  /* bits consumed */
};

static inline void bitr_fill(struct bitr *r) {
  while (r->n <= 24) {
    uint32_t b = 0;
    if (r->p < r->end) {
      b = *r->p++;
    }
    r->acc |= b << (24 - r->n);
    r->n += 8;
  }
}

static inline void bitr_drop(struct bitr *r, unsigned len) {
  r->acc <<= len;
  r->n -= len;
  r->used += len;
}

static int huff_decode(const struct sensei_delta_set *set, unsigned level, uint16_t step, const uint8_t *in,
                       size_t in_len, uint16_t n, int32_t *uhat) {
  uint8_t syms[N_SYM];
  uint32_t count[HUFF_MAXLEN + 1];
  uint32_t first[HUFF_MAXLEN + 1];
  uint32_t limit[HUFF_MAXLEN + 1];
  uint16_t offs[HUFF_MAXLEN + 1];
  struct bitr r;
  unsigned c, s, l;
  uint16_t t;

  r.p = in + HDR;
  r.end = in + in_len;
  r.acc = 0;
  r.n = 0;
  r.used = 0;

  for (c = 0; c < N_CH; c++) {
    const uint8_t *len = set->len[level][c];
    int32_t *uc = uhat + (size_t)c * n;
    uint32_t code = 0;
    unsigned o = 0;

    memset(count, 0, sizeof(count));
    for (s = 0; s < N_SYM; s++) {
      if (len[s] < 1 || len[s] > HUFF_MAXLEN) {
        return SENSEI_DELTA_E_CFG;
      }
      count[len[s]]++;
    }
    for (l = 1; l <= HUFF_MAXLEN; l++) {
      code = (code + count[l - 1]) << 1;
      first[l] = code;
      limit[l] = (code + count[l]) << (HUFF_MAXLEN - l);
      offs[l] = (uint16_t)o;
      o += count[l];
    }
    for (l = 1; l <= HUFF_MAXLEN; l++) { /* symbols sorted by (len, sym) */
      for (s = 0; s < N_SYM; s++) {
        if (len[s] == l) {
          syms[offs[l]++] = (uint8_t)s;
        }
      }
    }
    for (l = 1; l <= HUFF_MAXLEN; l++) {
      offs[l] = (uint16_t)(offs[l] - count[l]);
    }

    for (t = 1; t < n; t++) {
      uint32_t peek;
      uint32_t sy;
      bitr_fill(&r);
      peek = r.acc >> (32 - HUFF_MAXLEN);
      for (l = 1; l <= HUFF_MAXLEN && peek >= limit[l]; l++) {
      }
      if (l > HUFF_MAXLEN) {
        return SENSEI_DELTA_E_CORRUPT;
      }
      sy = syms[offs[l] + (peek >> (HUFF_MAXLEN - l)) - first[l]];
      bitr_drop(&r, l);
      if (sy == ESC) {
        bitr_fill(&r);
        uc[t] = (int32_t)(r.acc >> 16);
        bitr_drop(&r, 16);
      } else {
        uc[t] = uc[t - 1] + ((int32_t)sy - Q_MAX) * (int32_t)step;
      }
      if (r.used > (in_len - HDR) * 8u) {
        return SENSEI_DELTA_E_CORRUPT;
      }
    }
  }
  /* All bytes used, padding below 8 bits and zero. */
  {
    size_t total = (in_len - HDR) * 8u;
    if (total - r.used >= 8) {
      return SENSEI_DELTA_E_CORRUPT;
    }
    if (total > r.used) {
      bitr_fill(&r);
      if ((r.acc >> (32 - (unsigned)(total - r.used))) != 0) {
        return SENSEI_DELTA_E_CORRUPT;
      }
    }
  }
  return SENSEI_DELTA_OK;
}

#endif /* SENSEI_DELTA_ENABLE_HUFFMAN */

/* ---- public encode / decode ---- */

int sensei_delta_encode(const struct sensei_delta_tables *tab, const struct sensei_delta_cfg *cfg, const uint16_t *u,
                        uint16_t n, uint8_t *out, size_t out_cap, size_t *out_len, uint8_t *scratch,
                        size_t scratch_len) {
  const struct sensei_delta_set *set = NULL;
  uint16_t step = 0;
  int rc;
  unsigned c;
  uint16_t t;
  size_t nsym;

  rc = check_cfg(tab, cfg, &set, &step);
  if (rc) {
    return rc;
  }
  if (u == NULL || out == NULL || out_len == NULL || (n > 1 && scratch == NULL)) {
    return SENSEI_DELTA_E_ARG;
  }
  if (n < 1 || n > SENSEI_DELTA_MAX_N) {
    return SENSEI_DELTA_E_N;
  }
  if (scratch_len < SENSEI_DELTA_SCRATCH_SIZE(n)) {
    return SENSEI_DELTA_E_SCRATCH;
  }
  if (out_cap < HDR + (cfg->entropy == SENSEI_DELTA_ENTROPY_RANS ? 4u : 0u)) {
    return SENSEI_DELTA_E_SIZE;
  }

  {
    SENSEI_DELTA_PROF_BEGIN(dpcm);
    nsym = (size_t)n - 1;
    for (c = 0; c < N_CH; c++) {
      const uint16_t *uc = u + (size_t)c * n;
      uint8_t *sc = scratch + c * nsym;
      int32_t prev = (int32_t)uc[0];
      int32_t half = (int32_t)(step / 2);
      out[2 * c] = (uint8_t)(uc[0] & 0xFF);
      out[2 * c + 1] = (uint8_t)(uc[0] >> 8);
      for (t = 1; t < n; t++) {
        int32_t d = (int32_t)uc[t] - prev;
        int32_t a = d < 0 ? -d : d;
        int32_t m = (a + half) / (int32_t)step;
        if (m <= Q_MAX) {
          int32_t q = d < 0 ? -m : m;
          sc[t - 1] = (uint8_t)(q + Q_MAX);
          prev += q * (int32_t)step;
        } else {
          sc[t - 1] = ESC;
          prev = (int32_t)uc[t];
        }
      }
    }
    SENSEI_DELTA_PROF_END(dpcm);
  }

  {
    SENSEI_DELTA_PROF_BEGIN(coder);
#if SENSEI_DELTA_ENABLE_RANS
    if (cfg->entropy == SENSEI_DELTA_ENTROPY_RANS) {
      rc = rans_encode(set, cfg->level, u, n, scratch, out, out_cap, out_len);
    } else
#endif
#if SENSEI_DELTA_ENABLE_HUFFMAN
    {
      rc = huff_encode(set, cfg->level, u, n, scratch, out, out_cap, out_len);
    }
#else
    {
      rc = SENSEI_DELTA_E_CFG;
    }
#endif
    SENSEI_DELTA_PROF_END(coder);
  }
  return rc;
}

int sensei_delta_decode(const struct sensei_delta_tables *tab, const struct sensei_delta_cfg *cfg, const uint8_t *in,
                        size_t in_len, uint16_t n, int32_t *uhat) {
  const struct sensei_delta_set *set = NULL;
  uint16_t step = 0;
  int rc;
  unsigned c;

  rc = check_cfg(tab, cfg, &set, &step);
  if (rc) {
    return rc;
  }
  if (in == NULL || uhat == NULL) {
    return SENSEI_DELTA_E_ARG;
  }
  if (n < 1 || n > SENSEI_DELTA_MAX_N) {
    return SENSEI_DELTA_E_N;
  }
  if (in_len < HDR) {
    return SENSEI_DELTA_E_CORRUPT;
  }
  for (c = 0; c < N_CH; c++) {
    uhat[(size_t)c * n] = (int32_t)in[2 * c] | ((int32_t)in[2 * c + 1] << 8);
  }
#if SENSEI_DELTA_ENABLE_RANS
  if (cfg->entropy == SENSEI_DELTA_ENTROPY_RANS) {
    return rans_decode(set, cfg->level, step, in, in_len, n, uhat);
  }
#endif
#if SENSEI_DELTA_ENABLE_HUFFMAN
  return huff_decode(set, cfg->level, step, in, in_len, n, uhat);
#else
  return SENSEI_DELTA_E_CFG;
#endif
}
