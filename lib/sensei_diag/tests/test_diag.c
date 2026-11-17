/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sensei_diag.h"

static int fails;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void test_crc(void) {
  /* the CRC-32 check value */
  CHECK(sensei_diag_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
  CHECK(sensei_diag_crc32(NULL, 0) == 0);
}

static void test_reset_map(void) {
  /* hwinfo flags pass through */
  CHECK(sensei_diag_reset_map(SENSEI_RESET_WATCHDOG, true, false) == SENSEI_RESET_WATCHDOG);
  CHECK(sensei_diag_reset_map(SENSEI_RESET_PIN | SENSEI_RESET_SOFTWARE, true, false) ==
        (SENSEI_RESET_PIN | SENSEI_RESET_SOFTWARE));
  /* nRF: an empty reset reason register is a power-on (or brown-out) reset */
  CHECK(sensei_diag_reset_map(0, true, false) == SENSEI_RESET_POR);
  /* no hwinfo */
  CHECK(sensei_diag_reset_map(0, false, false) == SENSEI_RESET_UNKNOWN);
  CHECK(sensei_diag_reset_map(SENSEI_RESET_WATCHDOG, false, false) == SENSEI_RESET_UNKNOWN);
  /* a recorded fault adds a bit and keeps the hardware cause */
  CHECK(sensei_diag_reset_map(SENSEI_RESET_SOFTWARE, true, true) == (SENSEI_RESET_SOFTWARE | SENSEI_RESET_FAULT));
  CHECK(sensei_diag_reset_map(0, true, true) == (SENSEI_RESET_POR | SENSEI_RESET_FAULT));
  /* bits above the hwinfo range are dropped */
  CHECK(sensei_diag_reset_map(0x80000000u | SENSEI_RESET_PIN, true, false) == SENSEI_RESET_PIN);
  /* the bit positions are the ones of Zephyr hwinfo.h */
  CHECK(SENSEI_RESET_PIN == 1u && SENSEI_RESET_WATCHDOG == 0x10u && SENSEI_RESET_CPU_LOCKUP == 0x100u);
  CHECK(SENSEI_RESET_FLASH == 0x10000u);
}

static void test_reset_format(void) {
  char buf[64];

  CHECK(sensei_diag_reset_format(0, buf, sizeof(buf)) == 1 && strcmp(buf, "-") == 0);
  CHECK(sensei_diag_reset_format(SENSEI_RESET_WATCHDOG, buf, sizeof(buf)) == 3 && strcmp(buf, "WDT") == 0);
  CHECK(sensei_diag_reset_format(SENSEI_RESET_SOFTWARE | SENSEI_RESET_FAULT, buf, sizeof(buf)) == 10 &&
        strcmp(buf, "SOFT|FAULT") == 0);
  /* a bit without a name is skipped */
  CHECK(sensei_diag_reset_format(1u << 20, buf, sizeof(buf)) == 1 && strcmp(buf, "-") == 0);
  /* cut to fit, terminated, the full length is returned */
  char small[6];

  memset(small, 'x', sizeof(small));
  CHECK(sensei_diag_reset_format(SENSEI_RESET_SOFTWARE | SENSEI_RESET_FAULT, small, sizeof(small)) == 10);
  CHECK(strcmp(small, "SOFT|") == 0);
  CHECK(sensei_diag_reset_format(SENSEI_RESET_POR, NULL, 0) == 3);
  CHECK(strcmp(sensei_diag_reset_name(4), "WDT") == 0 && sensei_diag_reset_name(31)[0] == '\0');
}

static struct sensei_diag_fault sample(void) {
  struct sensei_diag_fault f = {
      .reason = SENSEI_FAULT_CPU_EXCEPTION,
      .flags = SENSEI_FAULT_FLAG_COREDUMP,
      .last_job = 7,
      .pc = 0x00012345,
      .lr = 0x0001ABCD,
      .status = 0x00000082,
      .uptime_s = 86400,
      .image_id = 0xDEADBEEF,
      .boot_count = 12,
  };

  strcpy(f.thread, "app_wq");
  return f;
}

static void test_fault_codec(void) {
  struct sensei_diag_fault f = sample(), g;
  uint8_t b[SENSEI_DIAG_FAULT_BYTES];

  sensei_diag_fault_pack(&f, b);
  CHECK(sensei_diag_fault_unpack(b, sizeof(b), &g) == 0);
  CHECK(memcmp(&f, &g, sizeof(f)) == 0);
  /* fixed little-endian layout: the host decoder relies on it */
  CHECK(b[0] == 0xF7 && b[1] == 0xD1 && b[2] == 1 && b[3] == 0);
  CHECK(b[4] == 0x45 && b[5] == 0x23 && b[6] == 0x01 && b[7] == 0x00);
  CHECK(memcmp(b + 32, "app_wq", 7) == 0);

  /* wrong size */
  CHECK(sensei_diag_fault_unpack(b, sizeof(b) - 1, &g) == -EINVAL);
  CHECK(sensei_diag_fault_unpack(NULL, sizeof(b), &g) == -EINVAL);
  /* not a record: zeroed or random RAM, other version */
  uint8_t z[SENSEI_DIAG_FAULT_BYTES] = {0};

  CHECK(sensei_diag_fault_unpack(z, sizeof(z), &g) == -ENOMSG);
  memset(z, 0xA5, sizeof(z));
  CHECK(sensei_diag_fault_unpack(z, sizeof(z), &g) == -ENOMSG);
  uint8_t v[SENSEI_DIAG_FAULT_BYTES];

  memcpy(v, b, sizeof(v));
  v[2] = 2;
  CHECK(sensei_diag_fault_unpack(v, sizeof(v), &g) == -ENOMSG);
  /* every single bit flip in the payload or the CRC is found */
  for (size_t i = 4; i < sizeof(b); i++) {
    for (int bit = 0; bit < 8; bit++) {
      memcpy(v, b, sizeof(v));
      v[i] ^= (uint8_t)(1u << bit);
      CHECK(sensei_diag_fault_unpack(v, sizeof(v), &g) == -EBADMSG);
    }
  }
}

static void test_thread_name(void) {
  struct sensei_diag_fault f = sample(), g;
  uint8_t b[SENSEI_DIAG_FAULT_BYTES];

  /* longer than the field: cut to 15 characters */
  memset(f.thread, 'a', sizeof(f.thread));
  sensei_diag_fault_pack(&f, b);
  CHECK(sensei_diag_fault_unpack(b, sizeof(b), &g) == 0);
  CHECK(strlen(g.thread) == SENSEI_DIAG_NAME_LEN - 1);
  /* not printable: replaced */
  memset(&f.thread, 0, sizeof(f.thread));
  f.thread[0] = 'x';
  f.thread[1] = '\n';
  f.thread[2] = (char)0xC3;
  sensei_diag_fault_pack(&f, b);
  CHECK(sensei_diag_fault_unpack(b, sizeof(b), &g) == 0);
  CHECK(strcmp(g.thread, "x??") == 0);
  /* empty */
  memset(&f.thread, 0, sizeof(f.thread));
  sensei_diag_fault_pack(&f, b);
  CHECK(sensei_diag_fault_unpack(b, sizeof(b), &g) == 0 && g.thread[0] == '\0');
}

static void test_reason_name(void) {
  CHECK(strcmp(sensei_diag_fault_reason_name(SENSEI_FAULT_CPU_EXCEPTION), "exception") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(SENSEI_FAULT_STACK_CHK_FAIL), "stack") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(SENSEI_FAULT_MODEM), "modem") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(16), "mem-generic") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(36), "usage-undefined-instruction") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(44), "secure-lazy-state-error") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(45), "arch") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(9), "unknown") == 0);
  CHECK(strcmp(sensei_diag_fault_reason_name(SENSEI_FAULT_UNKNOWN), "unknown") == 0);
}

static void test_boot_state(void) {
  uint8_t s[SENSEI_DIAG_STATE_BYTES];
  bool had = true;

  /* random RAM at power-up: the count starts at 1 */
  memset(s, 0xA5, sizeof(s));
  CHECK(sensei_diag_boot_count_next(s, &had) == 1 && !had);
  /* then it counts */
  CHECK(sensei_diag_boot_count_next(s, &had) == 2 && had);
  CHECK(sensei_diag_boot_count_next(s, NULL) == 3);
  /* zeroed RAM is not valid either */
  memset(s, 0, sizeof(s));
  CHECK(sensei_diag_boot_count_next(s, &had) == 1 && !had);
  /* a damaged state starts again */
  sensei_diag_boot_count_next(s, NULL);
  s[5] ^= 0x40;
  CHECK(sensei_diag_boot_count_next(s, &had) == 1 && !had);
  /* saturates */
  memset(s, 0, sizeof(s));
  sensei_diag_boot_count_next(s, NULL);
  s[4] = s[5] = s[6] = s[7] = 0xFF;
  uint32_t crc = sensei_diag_crc32(s, 8);

  s[8] = (uint8_t)crc;
  s[9] = (uint8_t)(crc >> 8);
  s[10] = (uint8_t)(crc >> 16);
  s[11] = (uint8_t)(crc >> 24);
  CHECK(sensei_diag_boot_count_next(s, &had) == UINT32_MAX && had);
  CHECK(sensei_diag_boot_count_next(s, &had) == UINT32_MAX && had);
}

int main(void) {
  test_crc();
  test_reset_map();
  test_reset_format();
  test_fault_codec();
  test_thread_name();
  test_reason_name();
  test_boot_state();
  if (fails != 0) {
    printf("%d check(s) failed\n", fails);
    return 1;
  }
  printf("sensei_diag: all checks passed\n");
  return 0;
}
