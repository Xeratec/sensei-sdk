# MAX77654 Driver

Philipp Schilk
PBL, D-ITET, ETH Zürich

Generic (Hardware/OS agnostic) driver for the MAX77654 PMIC IC.

## Usage

```c
// Create a handle struct, providing all relevant hardware-specific functions
// and configuration:
struct max77654_h h = {
  .read_regs = read_regs,
  .write_regs = write_regs,
  .adc_read = adc_read,
  .log = drvrlog,
  // Configuration:
  .conf = {
    .cid = 0xA,
    .main_bias_force_enable = true,
    .... many more settings .....
  },
};

// Configure the device:
if (max77654_init(&h) != E_MAX77654_SUCCESS) {
  printf("Driver Init failed!\n");
  return -1;
}

```

## Usage: Development

 - The driver and unit tests can be compiled and run via make:

```bash
# Compile + Run unit tests:
$ make all

# Clean:
$ make clean
```
 - All unit tests are found in `test/`. Additional test sources need to be added to the makefile.
 - All code should be formatted according to the `.clang-format` file provided:

```bash
$ make format
```

 - For LSP support using `clangd`, a `compile_commands.json` can be generated using [bear](https://github.com/rizsotto/Bear):

```bash 
$ make clean
$ bear -- make
```

## Origin and local patches (SENSEI)

Source: the MAX77654 driver by Philipp Schilk (PBL, D-ITET, ETH Zurich), Apache-2.0 (header of `src/max77654.c`,
SPDX tag in the files). It is used here by `NRF/system/pwr`. Local fixes, found by the `NRF/tests/pwr` tests and
checked against the MAX77654 data sheet (CNFG_CHG_A 0x20, ERCFLAG 0x05):

- `max77654_config_charger()`: `th_cool` was written to THM_COLD and `th_cold` to THM_COOL (swapped). The default
  config sets both to the same register value, so only a changed threshold showed it.
- `max77654_get_erc_flags()`: `wdt_off` was never set, `wdt_rst` was assigned twice (the second time with the WDT_OFF
  bit).

Test: `NRF/tests/pwr` (`test_60_charger_thermistor_thresholds_go_to_their_fields`,
`test_61_erc_flags_wdt_rst_and_wdt_off_are_separate`).
