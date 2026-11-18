<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# LPWAN shield: pin map

Source: the KiCad project of the shield (netlist exported with `kicad-cli`) and the Altium project of
the base board (nets read from the SchDoc files). Pin numbers below are connector pins of the
shield (J1 40-pin, J2 50-pin) or MCU pins. Items marked **verify** were not confirmed on hardware.

## Summary

- The nRF5340 reaches the shield with **SPI_A** (P0.08 to P0.11), **I2C_A** (P1.02, P1.03), the event
  line **P0.31**, the expander interrupt **P1.12**, and two nRF21540 lines (P0.27, P1.13).
- Shield power and the level shifter are switched by the **I/O expander** (TCAL6408, 0x20) on I2C_A.
  BUSY, the master strobe and the spare line also go through the expander.
- **SPI_A is shared with GAP9.** See the answer below.

## nRF9151 (U6)

| Function | nRF9151 pin | Net | Goes to |
|---|---|---|---|
| SPIS SCK | P0.18 | SPI_A_CLK | nRF5340 P0.08 (TXB0108 level shifter) |
| SPIS MOSI | P0.17 | SPI_A_MOSI | nRF5340 P0.09 |
| SPIS MISO | P0.16 | SPI_A_MISO | nRF5340 P0.10 |
| SPIS CSN | P0.19 | SPI_A_CS_nRF91 | nRF5340 P0.11 |
| EVENT out | P0.15 | nRF91_GPIO0 | nRF5340 P0.31 (J1.2) |
| BUSY out | P0.29 | nRF91_GPIO13 | expander P2 |
| ACK in | P0.31 | nRF91_GPIO16 | expander P0 |
| spare in | P0.30 | nRF91_GPIO18 | expander P1 |
| SPIM SCK (LR1121) | P0.03 | nRF91_SCK_B | LR1121 DIO2 (SCK) |
| SPIM MOSI | P0.06 | nRF91_MOSI_B | LR1121 DIO3 |
| SPIM MISO | P0.05 | nRF91_MISO_B | LR1121 DIO4 |
| LR1121 NSS | P0.01 | nRF91_nCS_B | LR1121 DIO1 (GPIO chip select) |
| LR1121 RESET | P0.00 | nRF91_GPIO14 | LR1121 NRESET |
| LR1121 BUSY | P0.04 | nRF91_GPIO12 | LR1121 BUSY (DIO0) |
| LR1121 event | P0.02 | nRF91_GPIO4 | LR1121 DIO9 (Modem-E event) |
| LTE antenna | ANT | RF50_LTE1 | RF connector J3 (RF50_LTE2), U.FL J4 (RF50_LTE4) |
| GNSS | GPS | RF50_GNSS1 | U.FL J5 (RF50_GNSS2) |
| GNSS antenna supply | COEX0 | EN_3V3_GNSS_ANT | load switch U5. The app sets `AT%XCOEX0=1,1,1565,1586` (NCS `modem_antenna`), **verify** on hardware |
| LTE/GNSS coexistence | COEX2 | GNSS_LTE_ACTIVE | LR1121 DIO7 |
| SIM | SIM_CLK/IO/RST | SIM_* | SIM holder U8 via ESD U7 |
| SWD, reset | SWDIO/SWDCLK/RESET | nRF91_SWDIO ... | debug header J12 and base board J2 |

Not connected: P0.07 to P0.14, P0.20 to P0.28, UART pins. **There is no UART on the nRF9151**: console
and logs use RTT through the debug header J12. The nRF9151 reset and SWD pins reach the base board J2
only on the pins named MSP_TCK/MSP_TMS/MSP_TRST there, which no nRF5340 GPIO drives. The nRF5340 can
only reset the nRF9151 by switching the shield power.

## LR1121 and RF

- LR1121 (U9): SPI to the nRF9151 as above. DIO5 (`RF_SW_CTRL`) and DIO6 (`RF_SW_EN`) drive the RF
  switch SKY13330 (U10). The Modem-E firmware sets them with `set_dio_as_rf_switch`. TCXO supply
  `VTCXO` from the LR1121 (3.0 V in the firmware), 32 kHz crystal on 32K_P/32K_N.
- DIO7 is tied to the nRF9151 COEX2 pin (`GNSS_LTE_ACTIVE`).
- LoRa antenna: U.FL J6 and J7 (through ESD diodes D1, D2), BLE range extender: U.FL J8 and J9.

## Base board and shield connectors

The shield plugs into the base board with J1 (40 pin) and J2 (50 pin). The pins carry the same net
names on both boards. Pins used:

| Base board net | nRF5340 pin | Shield connector pin | Use |
|---|---|---|---|
| SPI_A_CLK | P0.08 | J2.40 | SPI to nRF9151 |
| SPI_A_MOSI | P0.09 | J2.44 | |
| SPI_A_MISO | P0.10 | J2.34 | |
| SPI_A_CS | P0.11 | J2.46 | chip select of the nRF9151 |
| I2C_A_SCL | P1.02 | J2.30 | shield I/O expander |
| I2C_A_SDA | P1.03 | J2.32 | |
| nRF_P0.31 | P0.31 | J1.2 | EVENT from the nRF9151 |
| MSP_CS | P1.12 | J2.23 | expander interrupt (named `_I/O_exp_nINT` on the shield) |
| MSP_DR2 | P0.27 | J2.41 | nRF21540 TX_EN |
| MSP_DR1 | P1.13 | J2.25 | nRF21540 RX_EN |
| nRF_P0.07 | P0.07 | J1.26 | nRF21540 chip select (SPI_A_CS_nRF21) |
| SPI_GAP_SEL | P0.20 | (base board only) | GAP9 isolation switch |
| Vsys, Vbat, 1V8_BASE_BOARD | | J2.1 to J2.13 | power from the base board |

## I/O expander (U17, TCAL6408, I2C_A, address 0x20)

ADDR is tied to GND. Powered from the always-on 3.3 V (VCC_P) and 1.8 V (VCC_I).

| Pin | Net | Direction | Meaning |
|---|---|---|---|
| P0 | nRF91_GPIO16 | out | ACK: master strobe to the nRF9151 (P0.31) |
| P1 | nRF91_GPIO18 | out | spare line to the nRF9151 (P0.30) |
| P2 | nRF91_GPIO13 | in | BUSY from the nRF9151 (P0.29): high = busy |
| P3 | BB_MODE | out | buck-boost mode: high = forced PWM (1 M pull-up), low = power save |
| P4 | EN_3V3 | out | shield 3.3 V load switch U3 (1 M pull-up: on by default) |
| P5 | nRF21_nPD | out | nRF21540 power down (low = off) **verify** |
| P6 | nRF21_MODE | out | nRF21540 MODE **verify** |
| P7 | OE | out | output enable of the level shifter U12 TXB0108 (10 k and 12 k to GND: off by default) |
| INT | `_I/O_exp_nINT` | out | to the base board P1.12 (pull-up on the shield) |

The expander is **bring-up step one**: set EN_3V3 and BB_MODE, wait, set OE, then use SPI. The
net names of SCL and SDA at the expander symbol look swapped in the schematic (pin 12 "SCL" is on
`I2C_A_SDA`). The commissioning project talked to this expander, so this is most likely a symbol
naming error. **verify** on the board.

The expander INT goes to P1.12, but P1.12 is the base board's `MSP_CS` net: it is also routed to the
sensor shield's MSP430 interface. **verify** that nothing else on the stack drives it.

## Level shifter and 1.8 V domain

- The base board is 1.8 V, the nRF9151 side 3.3 V. U12 (TXB0108) shifts SPI_A (CLK, MOSI, MISO,
  CS_nRF91, CS_nRF21), EVENT (nRF91_GPIO0) and the nRF21540 TX_EN and RX_EN. Its OE comes from
  expander P7. The TXB0108 is auto-direction with weak drivers: the firmware runs SPI at 4 MHz.
- I2C_A is 1.8 V at the expander (VCC_I from the 1V8 rail).
- With OE low the EVENT line floats: the nRF5340 pin has a pull-down while the shield is off.
- **OE is low while GAP9 owns SPI_A.** GAP9's chip select (P0.11) and clock reach the nRF9151 SPIS through U12, and an
  armed SPIS drives MISO against SPIS3. So the client drives OE low when `BUS_GAP9` is claimed and high again when it is
  released or an exchange starts (`level_update()` in `lpwan_client.c`). While OE is low the EVENT line is not seen: the
  client reads it again when OE goes high. **verify** with a scope: OE (expander P7), P0.11, P0.10 during a GAP9 job with
  the shield on (LTE mode).
- **CSN and SCK bias.** With OE low the TXB0108 leaves the nRF9151 SPIS CSN (P0.19) and SCK (P0.18) floating, and so does
  the nRF5340 when it parks P0.11 between exchanges. The board pinctrl (`sensei_lpwan_nrf9151_nrf9151_common-pinctrl.dtsi`)
  sets a pull-up on CSN (not selected) and a pull-down on SCK (mode 0 idle). **verify** on the board: (1) with the shield
  on, OE low and nothing on SPI_A, CSN reads high and SCK low at the nRF9151 pins, and the log shows no `bad frame`
  bursts or spurious transfers; (2) at 4 MHz the CSN and SCK edges are clean with the pulls on. The nRF9151 pulls are
  about 13 k; the TXB0108 data sheet advises against pulls below about 50 k on its pins (check the current data sheet): if the edges are slow or the
  direction detection fails, remove the bias and pull CSN high on the board (about 100 k) or disarm the SPIS through the
  ACK line.

## Power (sheet 02-power)

```
J10 USB-C (5V_USB) --+
                     +--> U1 TPS2116 power mux --> V_OUT_PWR_SW --R12 0R--> V_IN_BUCK_BOOST
Vsys (base board) ---+                                                          |
                                                                       U2 TPS631011 buck-boost
                                                                                |
                                                          3V3_OUT_BUCK_BOOST --FB1--> 3V3_ALWAYS_ON
                                                                                |
                                                U3 TPS22916 (EN_3V3 = expander P4) --> 3V3
                                                                                |
                                      0.2 R each: 3V3_nRF91, 3V3_LR1121, 3V3_nRF21
```

- The mux U1 prefers 5 V from the shield's own USB-C (VIN1, PR1 divider R1 3.3 M) and falls back to
  Vsys from the base board PMIC (VIN2). In battery operation the shield runs from **Vsys**.
- The thesis reports that the base board's PMIC cannot supply the LTE modem peak current, so the
  shield was powered from USB. The schematic cannot tell the PMIC limit. What the firmware can do:
  - run **one radio at a time** (the state machine enforces it, LoRa and LTE never transmit together);
  - gate the shield (LoRa) so the buck-boost is off between uplinks;
  - keep PSM on for LTE (the modem is off the air most of the time) and use a long PSM TAU;
  - leave BB_MODE at forced PWM (the default) for the burst, power save is a Kconfig option;
  - use NB-IoT (lower peak than LTE-M) if the supply sags; limit TX power with the modem's
    `AT%XEMPR` (not done yet).
  The PMIC SBB current limit and the battery impedance decide. Measure Vsys during an LTE-M
  attach on the real stack. **verify**
- The three 0.2 R resistors R7, R9, R10 (and J11 header) allow measuring or cutting each module rail.

## nRF21540 range extender (BLE) (sheet 05)

- U11 nRF21540: SPI on SPI_A (own chip select J1.26, P0.07), TX_EN P0.27, RX_EN P1.13, nPD and MODE
  from expander P5/P6, antenna paths RF50_BLE1 to RF50_BLE5.
- **Clash with the energy markers**: P0.31 (EVENT) and P0.27 (TX_EN) are the marker pins. The core
  forbids markers together with the LPWAN client.
- **Can the nRF5340 BLE use it?** In principle yes: the NCS MPSL front-end module support
  (`CONFIG_MPSL_FEM_NRF21540_GPIO`) toggles TX_EN and RX_EN from the network core through
  PPI. Needs (1) P0.27 and P1.13 assigned to the network core in the devicetree, (2) PDN
  and MODE are on the expander, which MPSL cannot drive: use the GPIO-only variant, set PDN high and
  MODE once from the application core, (3) the RX_EN/TX_EN lines pass the TXB0108, check the
  switching delay against the FEM timing, (4) the RF path (U.FL ports) has to be used for BLE. Not
  implemented; not tested. The nRF21540 SPI is not needed in GPIO mode.

## GAP9 and SPI_A: the answer

**Yes, they share pins.** The nRF5340 pins P0.08 (SCK), P0.09 (MOSI), P0.10 (MISO), P0.11 (CS) are the
nets SPI_A_CLK, SPI_A_MOSI, SPI_A_MISO and SPI_A_CS. Evidence (base board, sheet 04-GAP_GPIO):

- SPI_A_* go to the board-to-board connector J2 (pins 40, 44, 34, 46) and so to the nRF9151.
- The same nets go through a quad analog switch (TMUX1112, `3b04...` in the SchDoc) to
  SPI_A_CLK_GAP, SPI_A_MOSI_GAP, SPI_A_MISO_GAP, SPI_A_CS_GAP, which are GAP9 SPI1_SCK (GPIO33),
  SPI1_SDO (GPIO38), SPI1_SDI (GPIO39) and SPI1_CS0 (GPIO34).
- The four switch select inputs are the net **SPI_GAP_SEL = P0.20**.

So the nRF9151 link and GAP9 sit on the same wires, and SPI_GAP_SEL only gates the GAP9 side. When
the switch is open, the bus is the nRF5340 and the shield. When it is closed, GAP9 is on the bus too.
The GAP9 firmware uses SPIS3 on these pins (GAP9 is master); the nRF9151 link needs an SPI master, so
it uses **SPIM4** (SPIM4 is only available on exactly P0.08 to P0.11).

Switching, as implemented:

1. `spi_acquire(timeout_ms)` (private to the client): waits for the bus mutex and for `sensei_busarb` (refuses while GAP9
   holds `BUS_GAP9`), makes sure the level shifter is on, drives SPI_GAP_SEL low (GAP9 isolated), applies the SPIM4
   default pin state, configures the chip select pin.
2. SPI transfer.
3. `spi_release()`: chip select to high impedance, SPIM4 pins to the low-power state, mutex and claim released. The
   GAP9 module drives SPI_GAP_SEL high again when it powers GAP9 and applies its own SPIS3 pin state.

A GAP9 job and an LPWAN exchange cannot overlap: `sensei_busarb` lets only one of them hold the bus. The polarity of SPI_GAP_SEL (high = GAP9 connected) is taken from the
GAP9 module; **verify** it with a scope. The nRF9151 pins on its side are not affected by the switch.

## Other findings

- **I2C address clash**: the sensor shield has a PCA6416A at 0x20 on I2C_A; the LPWAN expander is
  also 0x20. If both shields hang on the same I2C_A, they collide. **verify** how the shields stack.
- Pin P0.31 on the base board J1.2 is also free for the markers when no LPWAN shield is fitted.
