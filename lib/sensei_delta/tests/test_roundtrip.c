/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Round-trip and error tests of the engine against synthetic stand-in tables (generated/standin_tables.c, not trained).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensei_delta.h"
#include "standin_tables.h"

#define TAB (&standin_tables)

static int g_fail;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
      g_fail++;                                                                                                        \
    }                                                                                                                  \
  } while (0)

static uint32_t g_rng = 12345u;
static uint32_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}
static int32_t rnd_laplace(double scale) {
  double u = ((double)(rnd() >> 8) + 0.5) / 16777216.0;
  double v = -scale * log(u);
  return (int32_t)(rnd() & 1 ? v : -v);
}

/* Random walk per channel with rare jumps (escapes at small steps). */
static void make_window(uint16_t *u, uint16_t n, int jumpy) {
  unsigned c;
  uint16_t t;
  for (c = 0; c < SENSEI_DELTA_N_CH; c++) {
    int32_t v = 16384 + (int32_t)(rnd() % 32768u);
    for (t = 0; t < n; t++) {
      if (jumpy && rnd() % 16 == 0) {
        v = (int32_t)(rnd() % 65536u);
      } else {
        v += rnd_laplace(10.0 + 20.0 * (double)c);
      }
      if (v < 0) {
        v = 0;
      }
      if (v > 65535) {
        v = 65535;
      }
      u[(size_t)c * n + t] = (uint16_t)v;
    }
  }
}

static uint16_t g_u[7 * 512];
static int32_t g_uhat[7 * 512];
static uint8_t g_out[SENSEI_DELTA_MAX_PAYLOAD(512)];
static uint8_t g_scratch[SENSEI_DELTA_SCRATCH_SIZE(512)];

/* Encode, decode, check bound. Returns payload length (or 0 on failure). */
static size_t roundtrip(const struct sensei_delta_cfg *cfg, const uint16_t *u, uint16_t n, unsigned *n_esc) {
  size_t len = 0;
  int32_t half = (int32_t)(standin_tables.level_step[cfg->level] / 2);
  unsigned c;
  uint16_t t;
  int rc = sensei_delta_encode(TAB, cfg, u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch));
  CHECK(rc == 0);
  if (rc) {
    return 0;
  }
  CHECK(len >= SENSEI_DELTA_HEADER_BYTES && len <= SENSEI_DELTA_MAX_PAYLOAD(n));
  rc = sensei_delta_decode(TAB, cfg, g_out, len, n, g_uhat);
  CHECK(rc == 0);
  if (rc) {
    return 0;
  }
  if (n_esc) {
    *n_esc = 0;
  }
  for (c = 0; c < 7; c++) {
    for (t = 0; t < n; t++) {
      int32_t e = g_uhat[(size_t)c * n + t] - (int32_t)u[(size_t)c * n + t];
      if (e < 0) {
        e = -e;
      }
      if (e > half) {
        CHECK(e <= half);
        return 0;
      }
      if (n_esc && t > 0 && e == 0 && half > 0) {
        (*n_esc)++; /* upper bound only, exact hits are not all escapes */
      }
    }
    CHECK(g_uhat[(size_t)c * n] == (int32_t)u[(size_t)c * n]);
  }
  return len;
}

static void test_roundtrips(void) {
  static const uint16_t ns[] = {1, 2, 3, 4, 17, 100, 511, 512};
  unsigned si, lv, en, k, rep;
  for (si = 0; si < STANDIN_N_SETS; si += 2) {
    for (lv = 0; lv < STANDIN_N_LEVELS; lv++) {
      for (en = 0; en < 2; en++) {
        struct sensei_delta_cfg cfg = {(uint8_t)si, (uint8_t)lv, (uint8_t)en};
        for (k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
          for (rep = 0; rep < 6; rep++) {
            make_window(g_u, ns[k], rep & 1);
            CHECK(roundtrip(&cfg, g_u, ns[k], NULL) != 0);
          }
        }
      }
    }
  }
}

static void test_escapes(void) {
  unsigned lv, en;
  uint16_t n = 64;
  unsigned c;
  uint16_t t;
  for (lv = 0; lv < STANDIN_N_LEVELS; lv++) {
    for (en = 0; en < 2; en++) {
      struct sensei_delta_cfg cfg = {0, (uint8_t)lv, (uint8_t)en};
      int32_t s = standin_tables.level_step[lv];
      size_t len;
      /* all escapes: alternate the extremes (worst case size) */
      for (c = 0; c < 7; c++) {
        for (t = 0; t < n; t++) {
          g_u[c * n + t] = (t & 1) ? 65535 : 0;
        }
      }
      len = roundtrip(&cfg, g_u, n, NULL);
      CHECK(len != 0);
      for (c = 0; c < 7; c++) {
        for (t = 0; t < n; t++) {
          CHECK(g_uhat[c * n + t] == g_u[c * n + t]);
        }
      }
      /* around the escape threshold |q| = 64 (odd t: base + delta, even t: base) */
      {
        int32_t base = 20000;
        int32_t deltas[] = {
            64 * s, 64 * s + s / 2, 64 * s + s / 2 + 1, 65 * s, -64 * s, -(64 * s + s / 2), -(64 * s + s / 2 + 1), 0, 1,
            -1};
        for (c = 0; c < 7; c++) {
          g_u[c * n] = (uint16_t)base;
          for (t = 1; t < n; t++) {
            int32_t d = (t & 1) ? deltas[((t >> 1) + c) % 10] : 0;
            g_u[c * n + t] = (uint16_t)(base + d);
          }
        }
        CHECK(roundtrip(&cfg, g_u, n, NULL) != 0);
      }
    }
  }
}

static void test_errors(void) {
  struct sensei_delta_cfg cfg = {0, 1, 0};
  size_t len = 0, len2 = 0;
  uint16_t n = 200;
  int en, rc;
  size_t i;

  make_window(g_u, n, 1);
  cfg.entropy = 0;
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, 0, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_N);
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, 513, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_N);
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch,
                            SENSEI_DELTA_SCRATCH_SIZE(n) - 1) == SENSEI_DELTA_E_SCRATCH);
  cfg.set = 1; /* compiled out in the stand-in */
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_NOSET);
  CHECK(sensei_delta_decode(TAB, &cfg, g_out, 100, n, g_uhat) == SENSEI_DELTA_E_NOSET);
  cfg.set = STANDIN_N_SETS;
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_CFG);
  cfg.set = 0;
  cfg.level = STANDIN_N_LEVELS;
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_CFG);
  cfg.level = 1;
  cfg.entropy = 2;
  CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_CFG);
  CHECK(sensei_delta_encode(TAB, NULL, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_ARG);
  CHECK(sensei_delta_encode(NULL, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) ==
        SENSEI_DELTA_E_ARG);
  CHECK(sensei_delta_decode(NULL, &cfg, g_out, 100, n, g_uhat) == SENSEI_DELTA_E_ARG);

  for (en = 0; en < 2; en++) {
    unsigned flips = 0, detected = 0;
    static uint8_t buf[SENSEI_DELTA_MAX_PAYLOAD(512) + 1];
    cfg.entropy = (uint8_t)en;
    CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, g_out, sizeof(g_out), &len, g_scratch, sizeof(g_scratch)) == 0);
    /* exact fit works, one byte less fails */
    CHECK(sensei_delta_encode(TAB, &cfg, g_u, n, buf, len, &len2, g_scratch, sizeof(g_scratch)) == 0);
    CHECK(len2 == len && memcmp(buf, g_out, len) == 0);
    for (i = 0; i < len; i++) {
      rc = sensei_delta_encode(TAB, &cfg, g_u, n, buf, i, &len2, g_scratch, sizeof(g_scratch));
      CHECK(rc == SENSEI_DELTA_E_SIZE);
      if (rc != SENSEI_DELTA_E_SIZE) {
        break;
      }
    }
    /* truncated and extended payloads */
    CHECK(sensei_delta_decode(TAB, &cfg, g_out, len, n, g_uhat) == 0);
    for (i = 0; i < len; i++) {
      if (en == 0 || i + 1 != len) { /* Huffman may lose only padding */
        CHECK(sensei_delta_decode(TAB, &cfg, g_out, i, n, g_uhat) != 0);
      }
    }
    CHECK(sensei_delta_decode(TAB, &cfg, g_out, len - 1, n, g_uhat) != 0);
    memcpy(buf, g_out, len);
    buf[len] = 0;
    CHECK(sensei_delta_decode(TAB, &cfg, buf, len + 1, n, g_uhat) != 0);
    buf[len] = 0x5A;
    CHECK(sensei_delta_decode(TAB, &cfg, buf, len + 1, n, g_uhat) != 0);
    /* wrong n */
    CHECK(sensei_delta_decode(TAB, &cfg, g_out, len, (uint16_t)(n + 1), g_uhat) != 0);
    CHECK(sensei_delta_decode(TAB, &cfg, g_out, len, (uint16_t)(n - 1), g_uhat) != 0);
    /* bit flips: must not crash (ASan). Flips inside raw escape bytes are valid data. */
    for (i = SENSEI_DELTA_HEADER_BYTES * 8; i < len * 8; i++) {
      memcpy(buf, g_out, len);
      buf[i >> 3] ^= (uint8_t)(1u << (i & 7));
      flips++;
      if (sensei_delta_decode(TAB, &cfg, buf, len, n, g_uhat) != 0) {
        detected++;
      }
    }
    printf("  entropy %d: %u of %u single-bit flips detected\n", en, detected, flips);
    /* random garbage: no crash */
    for (i = 0; i < 200; i++) {
      size_t j, glen = SENSEI_DELTA_HEADER_BYTES + rnd() % 600;
      for (j = 0; j < glen; j++) {
        buf[j] = (uint8_t)rnd();
      }
      (void)sensei_delta_decode(TAB, &cfg, buf, glen, (uint16_t)(1 + rnd() % 512), g_uhat);
    }
  }
}

static void test_units(void) {
  unsigned c;
  CHECK(sensei_delta_to_units(TAB, 0, -1e30) == 0);
  CHECK(sensei_delta_to_units(TAB, 0, 1e30) == 65535);
  CHECK(sensei_delta_to_units(TAB, 0, -INFINITY) == 0);
  CHECK(sensei_delta_to_units(TAB, 0, INFINITY) == 65535);
  CHECK(sensei_delta_to_units(TAB, 0, NAN) == 0);
  for (c = 0; c < 7; c++) {
    double lo = standin_tables.lo[c];
    double k = standin_tables.k[c];
    double range = 32768.0 / k;
    double x;
    uint16_t prev = 0;
    CHECK(sensei_delta_to_units(TAB, c, lo) == 16384);
    CHECK(sensei_delta_to_units(TAB, c, lo + range) == 49152);
    for (x = lo - 0.4 * range; x <= lo + 1.4 * range; x += range / 997.0) {
      uint16_t u = sensei_delta_to_units(TAB, c, x);
      double back = sensei_delta_from_units(TAB, c, u);
      CHECK(u >= prev);
      CHECK(fabs(back - x) <= 0.5 / k * 1.0000001);
      prev = u;
    }
    CHECK(fabs(sensei_delta_from_units(TAB, c, 16384) - lo) < 1e-12);
  }
}

static void test_prefilter(void) {
  struct sensei_delta_prefilter f;
  uint16_t in[7], out[7];
  unsigned c, i;
  int32_t ref[7];
  int primed = 0;

  sensei_delta_prefilter_reset(&f);
  CHECK(!f.primed);
  for (i = 0; i < 20000; i++) {
    for (c = 0; c < 7; c++) {
      if (i < 50) {
        in[c] = (uint16_t)(1000u * (c + 1));
      } else if (i % 400 < 200) {
        in[c] = (uint16_t)(rnd() % 65536u);
      } else {
        in[c] = 65535;
      }
    }
    sensei_delta_prefilter_step(TAB, &f, in, out);
    for (c = 0; c < 7; c++) {
      /* independent reference with floor division */
      if (!primed) {
        ref[c] = in[c];
      } else {
        long long p = (long long)standin_tables.prefilter_a[c] * ((long long)in[c] - ref[c]);
        long long q = p + 16384;
        long long fl = q >= 0 ? q / 32768 : -((-q + 32767) / 32768);
        long long prev = ref[c];
        ref[c] = (int32_t)(ref[c] + fl);
        /* output lies between the previous output and the input */
        if (in[c] >= prev) {
          CHECK(ref[c] >= prev && ref[c] <= in[c]);
        } else {
          CHECK(ref[c] <= prev && ref[c] >= in[c]);
        }
      }
      CHECK(out[c] == ref[c]);
      if (i == 0) {
        CHECK(out[c] == in[c]); /* first sample passes through */
      }
    }
    primed = 1;
  }
  /* constant input stays constant; step response converges */
  sensei_delta_prefilter_reset(&f);
  for (c = 0; c < 7; c++) {
    in[c] = 30000;
  }
  for (i = 0; i < 10; i++) {
    sensei_delta_prefilter_step(TAB, &f, in, out);
    for (c = 0; c < 7; c++) {
      CHECK(out[c] == 30000);
    }
  }
  for (c = 0; c < 7; c++) {
    in[c] = 40000;
  }
  for (i = 0; i < 200; i++) {
    sensei_delta_prefilter_step(TAB, &f, in, out);
  }
  for (c = 0; c < 7; c++) {
    CHECK(out[c] >= 40000 - 2 && out[c] <= 40000);
  }
  /* in-place and reset */
  sensei_delta_prefilter_step(TAB, &f, in, in);
  sensei_delta_prefilter_reset(&f);
  in[0] = 123;
  sensei_delta_prefilter_step(TAB, &f, in, out);
  CHECK(out[0] == 123);
}

int main(void) {
  test_units();
  test_prefilter();
  test_roundtrips();
  test_escapes();
  test_errors();
  if (g_fail) {
    printf("test_roundtrip: %d failures\n", g_fail);
    return 1;
  }
  printf("test_roundtrip: PASS\n");
  return 0;
}
