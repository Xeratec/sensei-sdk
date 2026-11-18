/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_sim.h"

#include <string.h>

size_t lpwan_sim_pack(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out) {
  if (len > LPWAN_SIM_MAX_PAYLOAD) {
    return 0;
  }
  out[0] = type;
  out[1] = (uint8_t)(len & 0xFFu);
  out[2] = (uint8_t)(len >> 8);
  if (len > 0) {
    memcpy(out + LPWAN_SIM_HEADER, payload, len);
  }
  return LPWAN_SIM_HEADER + len;
}

int lpwan_sim_rx_next(const struct lpwan_sim_rx *r, uint8_t *type, const uint8_t **payload, size_t *len) {
  size_t n;

  if (r->n < LPWAN_SIM_HEADER) {
    return 0;
  }
  n = (size_t)r->buf[1] | ((size_t)r->buf[2] << 8);
  if (n > LPWAN_SIM_MAX_PAYLOAD) {
    return -1;
  }
  if (r->n < LPWAN_SIM_HEADER + n) {
    return 0;
  }
  *type = r->buf[0];
  *payload = r->buf + LPWAN_SIM_HEADER;
  *len = n;
  return 1;
}

void lpwan_sim_rx_pop(struct lpwan_sim_rx *r) {
  size_t n;

  if (r->n < LPWAN_SIM_HEADER) {
    return;
  }
  n = LPWAN_SIM_HEADER + ((size_t)r->buf[1] | ((size_t)r->buf[2] << 8));
  if (n > r->n) {
    n = r->n;
  }
  memmove(r->buf, r->buf + n, r->n - n);
  r->n -= n;
}

/* ---- AES-128 and CMAC for the join MIC of the simulated radio ---- */

static uint8_t sbox[256];

static uint8_t rotl8(uint8_t x, unsigned n) { return (uint8_t)((x << n) | (x >> (8u - n))); }

static void make_sbox(void) {
  uint8_t p = 1, q = 1;

  if (sbox[0] != 0) {
    return;
  }
  do { /* walk the field GF(2^8): p runs through the elements, q is its inverse */
    p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80u) ? 0x1Bu : 0u));
    q ^= (uint8_t)(q << 1);
    q ^= (uint8_t)(q << 2);
    q ^= (uint8_t)(q << 4);
    q ^= (q & 0x80u) ? 0x09u : 0u;
    sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63u);
  } while (p != 1);
  sbox[0] = 0x63;
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u)); }

static void aes_expand(const uint8_t key[16], uint8_t rk[176]) {
  uint8_t rcon = 1;

  memcpy(rk, key, 16);
  for (unsigned i = 16; i < 176; i += 4) {
    uint8_t t[4];

    memcpy(t, rk + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t x = t[0];

      t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
      t[1] = sbox[t[2]];
      t[2] = sbox[t[3]];
      t[3] = sbox[x];
      rcon = xtime(rcon);
    }
    for (unsigned j = 0; j < 4; j++) {
      rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
  }
}

static void aes_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];

  for (unsigned i = 0; i < 16; i++) {
    s[i] = (uint8_t)(in[i] ^ rk[i]);
  }
  for (unsigned round = 1; round <= 10; round++) {
    uint8_t t[16];

    for (unsigned c = 0; c < 4; c++) { /* SubBytes and ShiftRows: byte (row r, column c) comes from column c + r */
      for (unsigned r = 0; r < 4; r++) {
        t[4 * c + r] = sbox[s[4 * ((c + r) % 4) + r]];
      }
    }
    if (round < 10) { /* MixColumns */
      for (unsigned c = 0; c < 4; c++) {
        const uint8_t *a = t + 4 * c;
        uint8_t all = (uint8_t)(a[0] ^ a[1] ^ a[2] ^ a[3]);

        s[4 * c + 0] = (uint8_t)(a[0] ^ all ^ xtime((uint8_t)(a[0] ^ a[1])));
        s[4 * c + 1] = (uint8_t)(a[1] ^ all ^ xtime((uint8_t)(a[1] ^ a[2])));
        s[4 * c + 2] = (uint8_t)(a[2] ^ all ^ xtime((uint8_t)(a[2] ^ a[3])));
        s[4 * c + 3] = (uint8_t)(a[3] ^ all ^ xtime((uint8_t)(a[3] ^ a[0])));
      }
    } else {
      memcpy(s, t, 16);
    }
    for (unsigned i = 0; i < 16; i++) {
      s[i] = (uint8_t)(s[i] ^ rk[16 * round + i]);
    }
  }
  memcpy(out, s, 16);
}

/* RFC 4493: a subkey is the block shifted left by one bit, with 0x87 xored in if a bit fell out. */
static void cmac_subkey(const uint8_t in[16], uint8_t out[16]) {
  for (unsigned i = 0; i < 16; i++) {
    out[i] = (uint8_t)((in[i] << 1) | (i < 15 ? in[i + 1] >> 7 : 0));
  }
  if (in[0] & 0x80u) {
    out[15] ^= 0x87u;
  }
}

void lpwan_sim_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]) {
  uint8_t rk[176], zero[16] = {0}, l[16], k1[16], k2[16], x[16] = {0}, last[16] = {0};
  size_t blocks = (len + 15) / 16;
  bool whole = len != 0 && len % 16 == 0;

  make_sbox();
  aes_expand(key, rk);
  aes_encrypt(rk, zero, l);
  cmac_subkey(l, k1);
  cmac_subkey(k1, k2);
  if (blocks == 0) {
    blocks = 1;
  }
  for (size_t b = 0; b + 1 < blocks; b++) {
    for (unsigned i = 0; i < 16; i++) {
      x[i] ^= msg[16 * b + i];
    }
    aes_encrypt(rk, x, x);
  }
  if (whole) {
    for (unsigned i = 0; i < 16; i++) {
      last[i] = (uint8_t)(msg[16 * (blocks - 1) + i] ^ k1[i]);
    }
  } else {
    size_t rest = len - 16 * (blocks - 1);

    if (rest > 0) {
      memcpy(last, msg + 16 * (blocks - 1), rest);
    }
    last[rest] = 0x80;
    for (unsigned i = 0; i < 16; i++) {
      last[i] ^= k2[i];
    }
  }
  for (unsigned i = 0; i < 16; i++) {
    x[i] ^= last[i];
  }
  aes_encrypt(rk, x, mac);
}

void lpwan_sim_join_mic(const uint8_t app_key[16], const uint8_t join_eui[8], const uint8_t dev_eui[8],
                        uint16_t dev_nonce, uint8_t mic[4]) {
  uint8_t m[19], mac[16];

  m[0] = 0x00; /* MHDR: join request */
  for (unsigned i = 0; i < 8; i++) {
    m[1 + i] = join_eui[7 - i];
    m[9 + i] = dev_eui[7 - i];
  }
  m[17] = (uint8_t)(dev_nonce & 0xFFu);
  m[18] = (uint8_t)(dev_nonce >> 8);
  lpwan_sim_cmac(app_key, m, sizeof(m), mac);
  memcpy(mic, mac, 4);
}
