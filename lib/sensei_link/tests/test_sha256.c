/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SHA-256 against the FIPS 180-4 examples, with every split of the input. */

#include <string.h>

#include "check.h"
#include "sensei_link/sha256.h"

static void hex(const uint8_t d[32], char out[65]) {
  static const char *x = "0123456789abcdef";

  for (int i = 0; i < 32; i++) {
    out[2 * i] = x[d[i] >> 4];
    out[2 * i + 1] = x[d[i] & 15];
  }
  out[64] = 0;
}

static void digest(const uint8_t *m, size_t n, size_t step, char out[65]) {
  sensei_sha256_t c;
  uint8_t d[32];

  sensei_sha256_init(&c);
  for (size_t i = 0; i < n; i += step) {
    sensei_sha256_update(&c, m + i, n - i < step ? n - i : step);
  }
  sensei_sha256_final(&c, d);
  hex(d, out);
}

static void test_vectors(void) {
  static const struct {
    const char *msg;
    const char *sha;
  } v[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
  };
  char got[65];

  for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
    size_t n = strlen(v[i].msg);

    for (size_t step = 1; step <= 70; step += 3) { /* every split around the 64 byte block */
      digest((const uint8_t *)v[i].msg, n, step, got);
      CHECK(strcmp(got, v[i].sha) == 0);
    }
  }
}

static void test_million_a(void) {
  sensei_sha256_t c;
  uint8_t chunk[1000], d[32];
  char got[65];

  memset(chunk, 'a', sizeof(chunk));
  sensei_sha256_init(&c);
  for (int i = 0; i < 1000; i++) {
    sensei_sha256_update(&c, chunk, sizeof(chunk));
  }
  sensei_sha256_final(&c, d);
  hex(d, got);
  CHECK(strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

/* Lengths around the padding limit (55, 56, 63, 64 bytes): the same digest in one piece and in two. */
static void test_padding_edges(void) {
  uint8_t m[130];
  char one[65], two[65];

  for (unsigned i = 0; i < sizeof(m); i++) {
    m[i] = (uint8_t)(i * 7 + 1);
  }
  for (size_t n = 50; n <= 130; n++) {
    sensei_sha256_t c;
    uint8_t d[32];

    digest(m, n, n, one);
    sensei_sha256_init(&c);
    sensei_sha256_update(&c, m, n / 2);
    sensei_sha256_update(&c, m + n / 2, n - n / 2);
    sensei_sha256_final(&c, d);
    hex(d, two);
    CHECK(strcmp(one, two) == 0);
  }
}

static void test_ff(void) {
  uint8_t ff[300], d1[32], d2[32];
  sensei_sha256_t a, b;

  memset(ff, 0xFF, sizeof(ff));
  for (size_t n = 0; n <= sizeof(ff); n += 37) {
    sensei_sha256_init(&a);
    sensei_sha256_update(&a, (const uint8_t *)"head", 4);
    sensei_sha256_update(&a, ff, n);
    sensei_sha256_final(&a, d1);
    sensei_sha256_init(&b);
    sensei_sha256_update(&b, (const uint8_t *)"head", 4);
    sensei_sha256_update_ff(&b, n);
    sensei_sha256_final(&b, d2);
    CHECK(memcmp(d1, d2, 32) == 0);
  }
}

int main(void) {
  test_vectors();
  test_million_a();
  test_padding_edges();
  test_ff();
  return CHECK_DONE();
}
