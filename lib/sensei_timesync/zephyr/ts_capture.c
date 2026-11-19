/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_capture.c
 * @brief Hardware timestamping of pin edges: GPIOTE IN event -> DPPI -> RTC CAPTURE + TIMER CAPTURE + EGU trigger.
 *
 * The edge time is captured in hardware; the EGU interrupt only reads the capture registers and calls the callback.
 * The GPIOTE channel comes from the instance of the Zephyr GPIO driver (shared allocator, no interrupt of GPIOTE
 * is enabled for the channel).
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_nrf.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>

#include "ts_internal.h"

LOG_MODULE_REGISTER(ts_capture, CONFIG_SENSEI_TIMESYNC_LOG_LEVEL);

#define PORT_ENTRY(node) {DEVICE_DT_GET(node), DT_PROP(node, port)},

static const struct {
  const struct device *dev;
  uint8_t port;
} gpio_ports[] = {DT_FOREACH_STATUS_OKAY(nordic_nrf_gpio, PORT_ENTRY)};

int ts_gpio_resolve(const struct gpio_dt_spec *pin, nrfx_gpiote_t **g, nrfx_gpiote_pin_t *abs_pin) {
  if (!pin || !pin->port) {
    return -EINVAL;
  }
  if (!device_is_ready(pin->port)) {
    return -ENODEV;
  }
  for (size_t i = 0; i < ARRAY_SIZE(gpio_ports); i++) {
    if (gpio_ports[i].dev == pin->port) {
      nrfx_gpiote_t *inst = gpio_nrf_gpiote_by_port_get(pin->port);

      if (!inst || !nrfx_gpiote_init_check(inst)) {
        return -ENODEV;
      }
      *g = inst;
      *abs_pin = NRF_GPIO_PIN_MAP(gpio_ports[i].port, pin->pin);
      return 0;
    }
  }
  return -EINVAL;
}

#if CONFIG_SENSEI_TIMESYNC_CAPTURE_CHANNELS > 0

struct cap {
  bool used;
  bool is_pin; /* GPIO capture (else a peripheral event) */
  nrfx_gpiote_t *g;
  nrfx_gpiote_pin_t pin;
  uint8_t gch;
  nrfx_gppi_handle_t h;
  uint32_t eep;
  sensei_ts_capture_cb_t cb;
  void *user;
};

static struct cap caps[CONFIG_SENSEI_TIMESYNC_CAPTURE_CHANNELS];

void ts_capture_isr(uint8_t k) {
  struct cap *c = &caps[k];

  if (!c->used || !c->cb) {
    return;
  }
  /* The RTC capture register is updated 6 PCLK16M cycles (375 ns) after the capture; the interrupt entry can
   * be faster than that. */
  k_busy_wait(1);
  uint32_t rtc = nrf_rtc_cc_get(TS_RTC, TS_CC_CAPTURE0 + k);
  uint32_t tim = nrf_timer_cc_get(TS_TIMER, (nrf_timer_cc_channel_t)(TS_CC_CAPTURE0 + k));

  if (sensei_ts_local_running()) {
    c->cb(k, ts_tb_capture_to_ns(rtc, tim), c->user);
  }
}

/* Connect an event to RTC CAPTURE + TIMER CAPTURE + EGU trigger of capture channel i and enable it. The channel
 * must be reserved (used) by the caller. */
static int cap_connect(uint8_t i, uint32_t eep, sensei_ts_capture_cb_t cb, void *user) {
  struct cap *c = &caps[i];

  if (nrfx_gppi_conn_alloc(eep, nrf_rtc_task_address_get(TS_RTC, nrf_rtc_capture_task_get(TS_CC_CAPTURE0 + i)), &c->h) <
      0) {
    return -ENODEV;
  }
  nrfx_gppi_ep_attach(nrf_timer_task_address_get(TS_TIMER, nrf_timer_capture_task_get(TS_CC_CAPTURE0 + i)), c->h);
  nrfx_gppi_ep_attach(nrf_egu_task_address_get(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_CAP0 + i)), c->h);
  c->eep = eep;
  c->cb = cb;
  c->user = user;
  nrf_egu_event_clear(TS_EGU, nrf_egu_triggered_event_get(TS_EGU_CH_CAP0 + i));
  nrfx_gppi_conn_enable(c->h);
  return 0;
}

int sensei_ts_capture_start(uint8_t channel, const struct gpio_dt_spec *pin, gpio_flags_t edge,
                            sensei_ts_capture_cb_t cb, void *user) {
  nrfx_gpiote_t *g;
  nrfx_gpiote_pin_t abs_pin;
  nrfx_gpiote_trigger_t trig;
  nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_NOPULL;
  uint8_t gch;
  int err;

  if (channel >= ARRAY_SIZE(caps) || !pin || !cb) {
    return -EINVAL;
  }
  if (edge == GPIO_INT_EDGE_RISING) {
    trig = NRFX_GPIOTE_TRIGGER_LOTOHI;
  } else if (edge == GPIO_INT_EDGE_FALLING) {
    trig = NRFX_GPIOTE_TRIGGER_HITOLO;
  } else if (edge == GPIO_INT_EDGE_BOTH) {
    trig = NRFX_GPIOTE_TRIGGER_TOGGLE;
  } else {
    return -EINVAL;
  }
  err = ts_gpio_resolve(pin, &g, &abs_pin);
  if (err) {
    return err;
  }
  if (pin->dt_flags & GPIO_PULL_UP) {
    pull = NRF_GPIO_PIN_PULLUP;
  } else if (pin->dt_flags & GPIO_PULL_DOWN) {
    pull = NRF_GPIO_PIN_PULLDOWN;
  }

  struct cap *c = &caps[channel];
  unsigned int key = irq_lock();

  if (c->used) {
    irq_unlock(key);
    return -EBUSY;
  }
  c->used = true; /* reserve */
  irq_unlock(key);

  (void)nrfx_gpiote_pin_uninit(g, abs_pin); /* the Zephyr GPIO driver may have configured the pin */
  if (nrfx_gpiote_channel_alloc(g, &gch) != 0) {
    err = -ENODEV;
    goto fail_res;
  }
  const nrfx_gpiote_trigger_config_t tcfg = {.trigger = trig, .p_in_channel = &gch};
  const nrfx_gpiote_input_pin_config_t icfg = {
      .p_pull_config = &pull, .p_trigger_config = &tcfg, .p_handler_config = NULL};

  if (nrfx_gpiote_input_configure(g, abs_pin, &icfg) != 0) {
    err = -EINVAL;
    goto fail_ch;
  }

  err = cap_connect(channel, nrfx_gpiote_in_event_address_get(g, abs_pin), cb, user);
  if (err) {
    goto fail_pin;
  }
  c->is_pin = true;
  c->g = g;
  c->pin = abs_pin;
  c->gch = gch;
  nrfx_gpiote_trigger_enable(g, abs_pin, false);
  return 0;

fail_pin:
  (void)nrfx_gpiote_pin_uninit(g, abs_pin);
fail_ch:
  (void)nrfx_gpiote_channel_free(g, gch);
fail_res:
  c->used = false;
  return err;
}

int sensei_ts_capture_event_start(uint8_t channel, uint32_t event_address, sensei_ts_capture_cb_t cb, void *user) {
  if (channel >= ARRAY_SIZE(caps) || !event_address || !cb) {
    return -EINVAL;
  }
  struct cap *c = &caps[channel];
  unsigned int key = irq_lock();

  if (c->used) {
    irq_unlock(key);
    return -EBUSY;
  }
  c->used = true; /* reserve */
  irq_unlock(key);

  int err = cap_connect(channel, event_address, cb, user);

  if (err) {
    c->used = false;
    return err;
  }
  c->is_pin = false;
  return 0;
}

int sensei_ts_capture_stop(uint8_t channel) {
  if (channel >= ARRAY_SIZE(caps)) {
    return -EINVAL;
  }
  struct cap *c = &caps[channel];

  if (!c->used || !c->cb) {
    return -EINVAL;
  }
  if (c->is_pin) {
    nrfx_gpiote_trigger_disable(c->g, c->pin);
  }
  nrfx_gppi_conn_disable(c->h);
  nrfx_gppi_ep_clear(nrf_timer_task_address_get(TS_TIMER, nrf_timer_capture_task_get(TS_CC_CAPTURE0 + channel)));
  nrfx_gppi_ep_clear(nrf_egu_task_address_get(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_CAP0 + channel)));
  nrfx_gppi_conn_free(c->eep, nrf_rtc_task_address_get(TS_RTC, nrf_rtc_capture_task_get(TS_CC_CAPTURE0 + channel)),
                      c->h);
  if (c->is_pin) {
    (void)nrfx_gpiote_pin_uninit(c->g, c->pin);
    (void)nrfx_gpiote_channel_free(c->g, c->gch);
  }
  unsigned int key = irq_lock();

  c->cb = NULL;
  c->used = false;
  irq_unlock(key);
  return 0;
}

#else /* no capture channels */

void ts_capture_isr(uint8_t k) { ARG_UNUSED(k); }

int sensei_ts_capture_start(uint8_t channel, const struct gpio_dt_spec *pin, gpio_flags_t edge,
                            sensei_ts_capture_cb_t cb, void *user) {
  ARG_UNUSED(channel);
  ARG_UNUSED(pin);
  ARG_UNUSED(edge);
  ARG_UNUSED(cb);
  ARG_UNUSED(user);
  return -EINVAL;
}

int sensei_ts_capture_event_start(uint8_t channel, uint32_t event_address, sensei_ts_capture_cb_t cb, void *user) {
  ARG_UNUSED(channel);
  ARG_UNUSED(event_address);
  ARG_UNUSED(cb);
  ARG_UNUSED(user);
  return -EINVAL;
}

int sensei_ts_capture_stop(uint8_t channel) {
  ARG_UNUSED(channel);
  return -EINVAL;
}

#endif
