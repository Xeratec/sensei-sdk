<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_charge

Software charge counter for a battery without a fuel gauge (the MAX77654 has none). Plain C, no Zephyr, no locking:
the caller serialises the calls and passes the uptime in ms. The Zephyr part (retained RAM, settings, PMIC samples) is
`NRF/system/pwr/pwr_charge.c`.

```
used = floor + sum(time in state x current above floor) + sum(events x charge per event) + self-discharge
```

- **States** (GAP9 powered, shield powered, BLE connected, ...) are ref-counted: two `sensei_charge_state(id, true)`
  need two `false`. **Events** are jobs of known charge (one sensor reading, one LoRa attempt, n bytes).
- The **table** comes from the application (`struct sensei_charge_table`): uA per state, nC per event, at 3.6 V. The
  rails sit behind a buck-boost, so the battery pays the value times 3600 / V_bat (`sensei_charge_set_vbat()`, called
  every 20 s). Units: 1 uA x 1 ms = 1 nC.
- The raw **ledger** (seconds per state, counts per event, since the declaration) is kept too, so that a host can apply
  a better table later.
- **Self-discharge** (Li-SOCl2): 1 %/year of the capacity at 20 degC, doubling per +10 degC (`sensei_charge_set_temp()`).
  The voltage never corrects the counter; it only sets `VOLTAGE_EOL` (`sensei_charge_set_eol()`, two brown-outs within
  24 h).
- **Report** (`sensei_charge_report()`): used mAh, percent of the capacity, 7-day moving average of the current,
  days left to the usable share (`usable_pct`), 1-sigma error (7 % of the counted charge, 20 % of the self-discharge,
  plus the 6 h that a restore may have lost), flags.
- **External power** (USB): the battery does not supply the node, so only the ledger counts; the self-discharge continues.
- **Sanity checks** (never change the counter): `sensei_charge_check_sample()` compares AMUX samples with the table per
  class (running mean, `CHECK_MISMATCH` beyond +-30 % once 200 samples are in; samples under 25 % or over 400 % of the
  expected current are rejected). `sensei_charge_floor_sample()` watches the idle current: three samples in a row more
  than the margin (2 mA) above the floor set `FLOOR_HIGH` and the excess is counted.
- **Li-ion**: `sensei_charge_liion_charge()` integrates the charge current, `sensei_charge_liion_done()` (charger says
  done) sets used to 0, `sensei_charge_liion_empty()` learns the capacity from a full-to-3.3 V discharge,
  `sensei_charge_liion_ocv()` pulls towards the open-circuit voltage below 20 % and above 80 %.
- **Record** (`sensei_charge_pack()`, 32 B, little endian, magic, version, CRC-16) for the retained RAM and the
  settings; layout in `sensei_charge.c`. The ledger has its own blob. `sensei_charge_boot()` picks retained RAM,
  then settings, then zero, and applies the swap rules: a power-on reset with a rested voltage 150 mV above the last
  one is a new battery (`AUTO_SWAP`), any other power-on reset with lost retained RAM is `UNCONFIRMED`.
  `sensei_charge_btn_press()`: three presses within 3 s, only in the first 5 min after a power-on reset.

Flags: `RESTORED`, `UNCONFIRMED`, `AUTO_SWAP`, `FLOOR_HIGH`, `CHECK_MISMATCH`, `VOLTAGE_EOL`, `CAPACITY_DEFAULT`,
`LIION_SYNCED`.

## Use

**Zephyr**: `CONFIG_SENSEI_CHARGE=y` (`CONFIG_SENSEI_PWR_CHARGE` selects it).

**Host or plain CMake**: `add_subdirectory(<sdk>/lib/sensei_charge sensei_charge)` and link `sensei_charge`.

Memory: about 420 B of RAM per counter, about 2.5 kB of flash on the Cortex-M33.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_charge/tests -B build && cmake --build build &&
ctest --test-dir build`. They cover ref counts, uptime wrap, V_bat scaling, self-discharge and temperature, the
report, the year-one budget of the C4 LoRa scenario, external power, the checks, the record and ledger, restore
precedence, the swap rules, Li-ion and the button.
