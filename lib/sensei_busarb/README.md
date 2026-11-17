<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_busarb

One owner at a time for the shared SPI_A pins and for the LTE current peaks of the SENSEI base board.

GAP9 (SPIS3) and the LPWAN shield's nRF9151 (SPIM4) use the same nRF5340 pins P0.08 to P0.11. The supply
cannot carry a GAP9 job and an LTE transmission together. GAP9 conflicts with every other user, the other
users do not conflict with each other. Every claim has a time limit, so a user that never releases cannot
block the others for ever.

| User | Meaning |
|---|---|
| `BUS_GAP9` | GAP9 powered or booting, SPI_A is theirs |
| `BUS_SPI` | one SPI exchange with the nRF9151 |
| `BUS_LTE_CONN` | LTE registration in progress |
| `BUS_LTE_TX` | LTE transmission in progress |

- `include/sensei_busarb/bus_arb.h`: the API. `bus_arb_*` is the pure logic (time is an argument, no lock).
  `bus_claim()`, `bus_release()`, `bus_is_held()` and `bus_claim_wait()` use one global instance with a lock
  and the uptime clock (`zephyr/bus_arb_zephyr.c`).
- `bus_set_listener()`: one callback is told when `BUS_GAP9` is claimed or released (on the thread of the claimer, it
  must not block). A claim that follows an expired one counts as a new claim and is reported too. The LPWAN client uses it to switch the level shifter of the shield off while GAP9 owns SPI_A.
- Zephyr: `CONFIG_SENSEI_BUSARB` (selected by `SENSEI_GAP9` and `SENSEI_LPWAN`).
- Host test: `cmake -S tests -B <builddir>`, or `make test-host` in the SDK root. It also compiles the Zephyr wrapper
  against a stub of the few kernel calls it uses (`tests/stub/zephyr/kernel.h`, fake clock).

Moved from the SENSEI IEQ node firmware (`nrf5340/src/core/bus_arb.*` and `app_bus.c`).
