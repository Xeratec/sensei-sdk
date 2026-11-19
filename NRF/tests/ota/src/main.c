/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The trial supervision of NRF/system/ota (sensei_ota.c) as it is built in an app (CONFIG_SENSEI_OTA, the SMP request
 * hook registered at boot). The pure decision (sensei_ota_trial.h) has its host test; this suite checks what the
 * module does with it: when the app's check is called, that an SMP request (mcumgr hook) is needed to confirm, that
 * a failed confirm is tried again, that the node resets at the end of the trial, and that a confirmation by the host
 * ends it. MCUboot's image state and the reset are FFF fakes (linker wrap, see CMakeLists.txt); time is the simulated
 * clock of native_sim, and every test of a scenario continues the timeline of the one before.
 *
 * One scenario per build (Kconfig of this folder, testcase.yaml): an image is confirmed once and a reset ends the
 * run. The reset fake never returns, as the real one: it parks the system work queue, which ends the scenario. */

#include <zephyr/fff.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

#include "ota/sensei_ota.h"

#if defined(CONFIG_SENSEI_TEST_E2E_CONFIRM)
#include <zephyr/settings/settings.h>

#include "sensei_e2e_settings.h"
#endif

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(bool, __wrap_boot_is_img_confirmed);
FAKE_VALUE_FUNC(int, __wrap_boot_write_img_confirmed);

void __wrap_sys_reboot(int type);

static bool image_confirmed; /* what MCUboot's trailer says */
static int confirm_failures; /* the next writes of the confirm flag fail */
static int reboots;
static int reboot_type;

static bool confirmed_fake(void) { return image_confirmed; }

static int write_confirmed_fake(void) {
  if (confirm_failures > 0) {
    confirm_failures--;
    return -EIO;
  }
  image_confirmed = true;
  return 0;
}

void __wrap_sys_reboot(int type) {
  reboots++;
  reboot_type = type;
  for (;;) {
    k_sleep(K_FOREVER); /* the real call does not return */
  }
}

/* The functions that wait for the confirmation (sensei_ota_on_confirmed()). */
static int confirmed_calls, late_calls;
static void confirmed_hook(void *user) {
  zassert_equal(user, &confirmed_calls);
  confirmed_calls++;
}
static void __maybe_unused late_hook(void *user) {
  ARG_UNUSED(user);
  late_calls++;
}

/* The app's check */
static int check_calls;
static bool check_result;

static bool app_check(void) {
  check_calls++;
  return check_result;
}

/* Wait until the uptime is s seconds (every trial step is a multiple of 5 s after the start). */
static void at_s(int s) {
  int64_t ms = (int64_t)s * 1000 - k_uptime_get();

  if (ms > 0) {
    k_sleep(K_MSEC(ms));
  }
}

/* An SMP request came in, on any transport: the mcumgr hook that CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK registers. */
static void __maybe_unused smp_request(void) {
  struct mgmt_evt_op_cmd_arg arg = {.group = 0, .id = 0, .op = 0};
  int32_t rc = 0;
  uint16_t group = 0;

  (void)mgmt_callback_notify(MGMT_EVT_OP_CMD_RECV, &arg, sizeof(arg), &rc, &group);
}

static void *setup(void) {
  RESET_FAKE(__wrap_boot_is_img_confirmed);
  RESET_FAKE(__wrap_boot_write_img_confirmed);
  __wrap_boot_is_img_confirmed_fake.custom_fake = confirmed_fake;
  __wrap_boot_write_img_confirmed_fake.custom_fake = write_confirmed_fake;
  image_confirmed = false;
  return NULL;
}

ZTEST_SUITE(ota_trial, NULL, setup, NULL, NULL, NULL);

#define WRITES() __wrap_boot_write_img_confirmed_fake.call_count

/* ---- the image id from the slot (the only flash access of the module) ---- */

#if defined(CONFIG_SENSEI_TEST_LATE_LINK)

static void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, v & 0xffff);
  put16(p + 2, v >> 16);
}

ZTEST(ota_trial, test_00_image_id_from_slot0) {
  const struct flash_area *fa;
  uint8_t img[32 + 16 + 4 + 4 + 32] = {0};

  zassert_ok(flash_area_open(PARTITION_ID(slot0_partition), &fa));
  zassert_ok(flash_area_erase(fa, 0, 4096));
  zassert_equal(sensei_ota_image_id(), 0, "an empty slot has no id");

  /* header, 16 B of image, TLV area with the SHA-256 TLV */
  put32(img, 0x96f3b83dU);
  put16(img + 8, 32);  /* header size */
  put16(img + 10, 0);  /* no protected TLVs */
  put32(img + 12, 16); /* image size */
  put16(img + 48, 0x6907);
  put16(img + 50, 4 + 4 + 32);
  put16(img + 52, 0x10);
  put16(img + 54, 32);
  for (int i = 0; i < 32; i++) {
    img[56 + i] = 0xa0 + i;
  }
  zassert_ok(flash_area_write(fa, 0, img, sizeof(img)));
  zassert_equal(sensei_ota_image_id(), 0xa3a2a1a0U, "first 4 bytes of the hash, little endian");

  img[0] ^= 1; /* no MCUboot image */
  zassert_ok(flash_area_erase(fa, 0, 4096));
  zassert_ok(flash_area_write(fa, 0, img, sizeof(img)));
  zassert_equal(sensei_ota_image_id(), 0);
  flash_area_close(fa);
}

#endif /* LATE_LINK */

/* ---- start ---- */

#if defined(CONFIG_SENSEI_TEST_E2E_CONFIRM)

#define PLAIN_KEY_NAME CONFIG_SENSEI_SECURE_SETTINGS_PREFIX "/e2e_priv"

static bool plain_key_stored(void) {
  uint8_t buf[SENSEI_SECURE_PRIV_SIZE];

  return settings_load_one(PLAIN_KEY_NAME, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
}

#endif

ZTEST(ota_trial, test_01_start_on_trial) {
#if defined(CONFIG_SENSEI_TEST_E2E_CONFIRM)
  /* A plain key of the old firmware: the key store seals it at its first start and keeps the plain record for a revert.
   * It registers with the OTA module (CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_ON_OTA), so the plain record goes when the
   * image is confirmed. */
  struct sensei_e2e_nv nv;
  uint8_t priv[SENSEI_SECURE_PRIV_SIZE], pub[SENSEI_SECURE_PUB_SIZE];

  zassert_ok(settings_subsys_init());
  zassert_ok(sensei_secure_keygen(sensei_secure_crypto_zephyr(), priv, pub));
  zassert_ok(settings_save_one(PLAIN_KEY_NAME, priv, sizeof(priv)));
  zassert_ok(sensei_e2e_settings_load(&nv));
  zassert_true(nv.have_priv && nv.priv_sealed);
  zassert_true(sensei_e2e_settings_plain_pending());
  zassert_true(plain_key_stored());
#endif
#if defined(CONFIG_SENSEI_TEST_ALREADY_CONFIRMED)
  image_confirmed = true;
  zassert_ok(sensei_ota_on_confirmed(confirmed_hook, &confirmed_calls));
  zassert_equal(confirmed_calls, 0, "nothing is called before sensei_ota_init()");
  zassert_ok(sensei_ota_init(app_check));
  zassert_equal(sensei_ota_state(), SENSEI_OTA_CONFIRMED);
  zassert_equal(confirmed_calls, 1, "at the start of an image that is confirmed already");
  zassert_ok(sensei_ota_init(app_check)); /* again: not twice */
  zassert_equal(confirmed_calls, 1);
  zassert_ok(sensei_ota_on_confirmed(late_hook, NULL));
  zassert_equal(late_calls, 1, "a function that registers later is called at once");
  zassert_equal(WRITES(), 0);
  return;
#endif
  zassert_ok(sensei_ota_on_confirmed(confirmed_hook, &confirmed_calls));
  zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL);
  check_result = !IS_ENABLED(CONFIG_SENSEI_TEST_UNHEALTHY);
#if defined(CONFIG_SENSEI_TEST_LATE_LINK)
  check_result = false;
#endif
#if defined(CONFIG_SENSEI_TEST_EARLY_LINK) || defined(CONFIG_SENSEI_TEST_UNHEALTHY) ||                                 \
    defined(CONFIG_SENSEI_TEST_HOST_CONFIRM) || defined(CONFIG_SENSEI_TEST_E2E_CONFIRM)
  smp_request(); /* before sensei_ota_init(): the hook was registered at boot */
#endif
  zassert_ok(sensei_ota_init(app_check));
  zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL);
  zassert_equal(WRITES(), 0);
  zassert_equal(confirmed_calls, 0);
}

/* ---- the check starts after the delay ---- */

ZTEST(ota_trial, test_02_no_check_before_the_delay) {
#if defined(CONFIG_SENSEI_TEST_ALREADY_CONFIRMED)
  at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S + 10);
  zassert_equal(check_calls, 0, "a confirmed image has no trial");
  zassert_equal(reboots, 0);
  zassert_equal(WRITES(), 0);
  zassert_equal(confirmed_calls, 1);
  return;
#endif
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S - 1);
  zassert_equal(check_calls, 0, "the check may start work: not before the delay");
  zassert_equal(WRITES(), 0);
  zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL);
}

#if defined(CONFIG_SENSEI_TEST_LATE_LINK)

/* The check fails: no confirm, the check is repeated. Then it passes, but no SMP request came yet. */
ZTEST(ota_trial, test_03_check_fails_then_passes_without_link) {
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 3);
  zassert_equal(check_calls, 1);
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 13);
  zassert_equal(check_calls, 3, "once per period");
  zassert_equal(WRITES(), 0);

  check_result = true;
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 28);
  zassert_true(check_calls >= 6);
  zassert_equal(WRITES(), 0, "a healthy image that no SMP request has reached is not confirmed");
  zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL);
  zassert_equal(reboots, 0);
}

/* A request arrives: the next period confirms. The first write fails (flash error): it is tried again. */
ZTEST(ota_trial, test_04_request_confirms_and_a_failed_write_is_retried) {
  int calls;

  confirm_failures = 1;
  smp_request();
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 28 + 5);
  zassert_equal(WRITES(), 1);
  zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL, "the write failed");
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 28 + 10);
  zassert_equal(WRITES(), 2, "tried again");
  zassert_equal(sensei_ota_state(), SENSEI_OTA_CONFIRMED);
  zassert_equal(confirmed_calls, 1, "called once, when the confirm write worked (not for the failed one)");

  /* the trial is over: no more checks, no more writes, no reset at the end of the trial time */
  calls = check_calls;
  at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S + 10);
  zassert_equal(check_calls, calls);
  zassert_equal(WRITES(), 2);
  zassert_equal(reboots, 0);
  zassert_equal(confirmed_calls, 1);
  zassert_ok(sensei_ota_on_confirmed(late_hook, NULL));
  zassert_equal(late_calls, 1, "a function that registers after the confirmation is called at once");
  zassert_equal(confirmed_calls, 1);
}

/* The table of functions: 4 places, one used by the test above; a function is called once. */
ZTEST(ota_trial, test_05_the_table_of_functions) {
  zassert_ok(sensei_ota_on_confirmed(late_hook, NULL)); /* latched: called, not stored */
  zassert_equal(late_calls, 2);
  zassert_equal(sensei_ota_on_confirmed(NULL, NULL), -EINVAL);
}

#endif /* LATE_LINK */

#if defined(CONFIG_SENSEI_TEST_E2E_CONFIRM)

/* The plain key record stays during the trial and goes when the image is confirmed (the self-confirm here). */
ZTEST(ota_trial, test_03_the_plain_key_goes_with_the_confirmation) {
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S - 1);
  zassert_true(plain_key_stored(), "a revert needs it");
  zassert_true(sensei_e2e_settings_plain_pending());
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 1);
  zassert_equal(sensei_ota_state(), SENSEI_OTA_CONFIRMED);
  zassert_equal(confirmed_calls, 1);
  zassert_false(plain_key_stored());
  zassert_false(sensei_e2e_settings_plain_pending());
}

#endif /* E2E_CONFIRM */

#if defined(CONFIG_SENSEI_TEST_EARLY_LINK)

/* The request came before sensei_ota_init(): the first check after the delay confirms. */
ZTEST(ota_trial, test_03_request_during_start_up_counts) {
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 1);
  zassert_equal(check_calls, 1);
  zassert_equal(WRITES(), 1);
  zassert_equal(sensei_ota_state(), SENSEI_OTA_CONFIRMED);
  zassert_equal(confirmed_calls, 1);
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 20);
  zassert_equal(check_calls, 1);
  zassert_equal(reboots, 0);
}

#endif /* EARLY_LINK */

#if defined(CONFIG_SENSEI_TEST_NO_LINK)

/* The check passes, no request comes. With the link rule the image stays on trial and the node resets when the trial
 * time is over (MCUboot goes back to the old image). Without the rule (SENSEI_OTA_CONFIRM_NEEDS_LINK=n) it confirms. */
ZTEST(ota_trial, test_03_without_request) {
  at_s(CONFIG_SENSEI_OTA_CONFIRM_DELAY_S + 1);
  if (IS_ENABLED(CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK)) {
    zassert_equal(WRITES(), 0);
    at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S - 1);
    zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL);
    zassert_equal(reboots, 0, "not before the trial time is over");
    at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S + 1);
    zassert_equal(reboots, 1, "the node resets, MCUboot reverts");
    zassert_equal(reboot_type, SYS_REBOOT_COLD);
    zassert_equal(WRITES(), 0);
    zassert_equal(confirmed_calls, 0, "reverted: never confirmed");
  } else {
    zassert_equal(WRITES(), 1);
    zassert_equal(sensei_ota_state(), SENSEI_OTA_CONFIRMED);
    zassert_equal(confirmed_calls, 1);
    at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S + 1);
    zassert_equal(reboots, 0);
  }
}

#endif /* NO_LINK */

#if defined(CONFIG_SENSEI_TEST_UNHEALTHY)

/* Requests come, the check never passes: the image is not confirmed and the node resets at the end of the trial. */
ZTEST(ota_trial, test_03_check_never_passes) {
  at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S - 1);
  zassert_true(check_calls > 10);
  zassert_equal(WRITES(), 0);
  zassert_equal(reboots, 0);
  smp_request();
  at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S + 1);
  zassert_equal(reboots, 1);
  zassert_equal(reboot_type, SYS_REBOOT_COLD);
  zassert_equal(WRITES(), 0);
  zassert_equal(confirmed_calls, 0);
}

#endif /* UNHEALTHY */

#if defined(CONFIG_SENSEI_TEST_HOST_CONFIRM)

/* SENSEI_OTA_CONFIRM=n: the node never confirms by itself and never calls the check. The host confirms over SMP (the
 * image group writes the flag itself): the supervision sees it and does not reset at the end of the trial time. */
ZTEST(ota_trial, test_03_only_the_host_confirms) {
  at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S / 2);
  zassert_equal(check_calls, 0, "no self-confirm: no check");
  zassert_equal(WRITES(), 0);
  zassert_equal(sensei_ota_state(), SENSEI_OTA_TRIAL);

  image_confirmed = true; /* what the SMP image confirm does */
  zassert_equal(sensei_ota_state(), SENSEI_OTA_CONFIRMED);
  at_s(CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S + 10);
  zassert_equal(reboots, 0, "confirmed by the host: no reset");
  zassert_equal(WRITES(), 0);
  zassert_equal(check_calls, 0);
  zassert_equal(confirmed_calls, 1, "the supervision saw the SMP confirm and called the function");
}

#endif /* HOST_CONFIRM */
