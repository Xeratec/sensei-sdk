<!--
SPDX-FileCopyrightText: 2024 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# SENSEI SDK (Smart Embedded Nano SEnsor Interface)

Software development kit for the SENSEI ecosystem.

This repository contains the SENSEI specific SDK for the NRF5340 and the GAP9.

**Currently the nRF Connect SDK v3.4.1 (Zephyr 4.4) is supported!**

## Getting Started

First, install the NRF Connect SDK and GAP9 SDK as described in [Install.md](Install.md).

Next, clone this repository:

```sh
git clone git@iis-git.ee.ethz.ch:sensei/software/sensei-sdk.git
cd sensei-sdk
git submodule update --init --recursive
```

Install pre-commit hooks.

```sh
source scripts/install-hooks.sh

```

Make sure to export the following environment variables.

```sh
export SENSEI_SDK_ROOT=$(pwd)

# Bash (Permanently)
echo "export SENSEI_SDK_ROOT=$(pwd)" >> ~/.bashrc

# Zsh (Permanently)
echo "export SENSEI_SDK_ROOT=$(pwd)" >> ~/.zshrc

# Fish (Permanently)
echo "set -x SENSEI_SDK_ROOT $(pwd)" >> ~/.config/fish/config.fish
```

## Examples
Each example contains two folders: `src_GAP9` and `src_NRF`. The former contains the GAP9 specific code, while the latter contains the NRF specific code.
First, flash the NRF application by opening it in Visual Studio Code using the NRF Connect SDK extension or use the terminal as described below.
**Make sure your `SENSEI_SDK_ROOT` environment variable is set to the path of the SENSEI SDK!**


### nRF: Use Visual Studio Code
Open the `sensei-sdk.code-workspace` file in Visual Studio Code. This will load the workspace with all relevant folders.
Furthermore, it will automatically set the the SENSEI board to to list of supported boards in the nRF Connect SDK extension.

1. Use the file explorer to navigate to the desired example folder and select "nRF Connect: Add Folder as Application" on the `src_NRF` folder.
2. In the "Applications" tab of the nRF Connect SDK extension, select the newly added application and click on "Add build configuration".
3. In the new window under "CMakePreset", select "Build for NRF5340 SENSEIV1C APP (build)", and select `v3.4.1` as SDK version and as Toolchain version.
4. Click on "Generate and Build" to build the application.
5. In the "Actions" tab click on "Flash" to flash the application to the board.

### nRF: Use Terminal
Make sure to export `ZEPHYR_BASE` pointing to the correct nRF Connect SDK folder and you activated the mamba/conda environment as described in [Install.md](Install.md).

To build and flash the nRF application, open a terminal in the `src_NRF` folder of the desired example and run the following commands:
```sh
cd <example>/src_NRF
west build -b nrf5340_senseiv1/nrf5340/cpuapp -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
west flash
```

The build uses sysbuild: it also builds the network core image where an example needs Bluetooth.
`-DEXTRA_ZEPHYR_MODULES=...` makes the SDK board known to sysbuild (the build presets set it).

### Board targets
| Target | Use |
|---|---|
| `nrf5340_senseiv1/nrf5340/cpuapp` | nRF5340 application core of the SENSEI base board |
| `nrf5340_senseiv1/nrf5340/cpunet` | nRF5340 network core (built by sysbuild as `ipc_radio`) |
| `nrf5340_senseiv1/nrf5340/cpuapp/bootloader` | application core variant for MCUboot with USB serial recovery |
| `sensei_lpwan_nrf9151/nrf9151/ns` | nRF9151 of the LPWAN shield, non-secure with TF-M |
| `sensei_lpwan_nrf9151/nrf9151` | nRF9151 of the LPWAN shield, secure image only |

Shields: `UT_SensorShield_v1`, `SENSEI_DebugBoard_v1` and `LPWAN_Shield_v1` (`west build --shield <name>`).
Flash partitions are devicetree partitions of the board (MCUboot slots included, MCUboot is off by default).

### GAP9: Use Terminal
Next, flash the GAP9 application by opening a terminal in the `src_GAP9` folder and running the following command:

```sh
gap init
gap run
```

In case you want to use GVSoC, modify the `sdk.config` and enable `CONFIG_PLATFORM_GVSOC=y`. Alternatively, run:

```sh
gap menuconfig
```
to configure the configuration with a graphical interface in the terminal.


## BareMetal Drivers
- **MAX77654**: Analog Devices Power Management IC. The power module (`CONFIG_SENSEI_PWR`) gives `pwr_get_status()` (battery voltage through the PMIC AMUX on P0.07, charger and input state) and `pwr_set_event_cb()` for nIRQ events (charger, input, button, battery level). It switches the PMIC rails on at boot, before the shield chips start, and takes the battery from one Kconfig choice (Li-SOCl2 primary cell by default, charger off), see [NRF/system/pwr](NRF/system/pwr/README.md).


## Zephyr Drivers
The nodes are in the board and shield devicetrees. The drivers turn on by themselves (Kconfig default `y` for an enabled node). The ST sensor drivers need the `hal_st` module in the west workspace.

| IC | Where | Driver |
|---|---|---|
| IS31FL3194 RGB LED | base board, I2C_A 0x53 | Zephyr `issi,is31fl3194` |
| LIS2DUXS12 accelerometer | base board, I2C_A 0x18 or 0x19, INT1 P0.23 | Zephyr `st,lis2duxs12`, wake-up and deep power-down in [NRF/drivers/sensor](NRF/drivers/sensor/README.md) |
| PCA6416A expander | sensor shield, I2C_B 0x20 | Zephyr `nxp,pca6416` (the SDK copy `gpio_pca64xxa` is gone) |
| TCAL6408 expander | LPWAN shield, I2C_A 0x20 | Zephyr `nxp,pcal6408a` |
| ISM330DHCX IMU | sensor shield, I2C_A 0x6A | Zephyr `st,ism330dhcx` |
| ILPS28QSW pressure | sensor shield, I2C_B 0x5C | SDK `st,ilps28qsw`, [NRF/drivers/sensor](NRF/drivers/sensor/README.md) |
| SCD41 CO2 | sensor shield, I2C_B 0x62, rail P0.04, switch P12 | SDK `sensei,scd41`, single shot, [NRF/drivers/sensor](NRF/drivers/sensor/README.md) |
| SGP41 VOC and NOx | sensor shield, I2C_B 0x59, rail P0.05, switch P7 | SDK `sensirion,sgp41`, raw ticks (index: `lib/sensei_gas_index`) |
| BME688 T, RH, p, gas | sensor shield, I2C_A 0x76 | SDK `bosch,bme688`, Bosch API, forced mode |
| AS7331 UV | sensor shield, I2C_B 0x74, switch P10 | SDK `ams,as7331`, uW/cm2 |
| BH1730FVC light | sensor shield, I2C_B 0x29 | SDK `sensei,bh1730` |
| MAX-M10S GNSS | sensor shield, I2C_B 0x42 | SDK `u-blox,max-m10s`, [NRF/drivers/gnss](NRF/drivers/gnss/README.md) |
| APS12804O PSRAM | base board, QSPI | SDK `apmemory,aps12804o`, disabled, [NRF/drivers/memory](NRF/drivers/memory/README.md) |
| nRF21540 FEM | LPWAN shield | Zephyr `nordic,nrf21540-fem`, node only, disabled |

The SCD41, SGP41, AS7331 and BH1730 drivers switch their rail or power-down through runtime device PM and are off by default (`CONFIG_SCD41_SENSEI`, `CONFIG_SGP41_SENSEI`, `CONFIG_AS7331_SENSEI`, `CONFIG_BH1730_SENSEI`, with `CONFIG_PM_DEVICE=y` and `CONFIG_PM_DEVICE_RUNTIME=y`). Vendor libraries are in [NRF/system/third_party](NRF/system/third_party/README.md).

All SDK drivers are untested on hardware. `example/drivers_test` probes every IC and puts it into its lowest-power state.


## System Libraries
- **Crypto**: Simplified crypto library on top of the PSA Crypto API (AES-GCM, SHA-256, HMAC, HKDF, ECDH and ECDSA on P-256), see [NRF/system/crypto](NRF/system/crypto/README.md).
  - Enable with `CONFIG_SENSEI_CRYPTO`, example in `example/crypto_api`.
- **Marker** (`CONFIG_SENSEI_MARKER`): two GPIO outputs for the digital inputs of a power analyzer, see [NRF/system/marker](NRF/system/marker/README.md).
- **Watchdog** (`CONFIG_SENSEI_WDT`): hardware watchdog fed only while the application is healthy, see [NRF/system/wdt](NRF/system/wdt/README.md).
- **Diagnostics** (`CONFIG_SENSEI_DIAG`): reset cause, boot counter and the record of the last fatal error in retained RAM, see [NRF/system/diag](NRF/system/diag/README.md).


## Portable Libraries (`lib/`)
Plain C libraries for nRF (Zephyr), GAP9 and host builds. Each folder has `include/`, `src/`, a `CMakeLists.txt` for host builds, `zephyr/` (Kconfig and CMake of the Zephyr library, hooked into `NRF/Kconfig` and `NRF/CMakeLists.txt`) and `tests/` (host tests). `make test-host` runs all of them.

| Library | Kconfig | What it does |
|---|---|---|
| [lib/sensei_delta](lib/sensei_delta/README.md) | `CONFIG_SENSEI_DELTA` | Delta coder engine (DPCM, rANS, Huffman); the application brings the trained tables |
| [lib/sensei_secure](lib/sensei_secure/README.md) | `CONFIG_SENSEI_SECURE` | End-to-end envelope (ECDH P-256, HKDF, AES-GCM, replay window) on the crypto library |
| [lib/sensei_link](lib/sensei_link/README.md) | `CONFIG_SENSEI_LINK` | Message framing (COBS and CRC streams, SPI frames, fragments), nanopb glue, `types.proto` and `lpwan.proto` |
| [lib/sensei_lpwan](lib/sensei_lpwan/README.md) | `CONFIG_SENSEI_LPWAN` | Client of the LPWAN shield's nRF9151 (LTE-M, NB-IoT, LoRaWAN) and the protocol cores of both sides |
| [lib/sensei_gap9](lib/sensei_gap9/README.md) | `CONFIG_SENSEI_GAP9` | Link between the nRF5340 and GAP9 (jobs and results over SPI, power sequencing, firmware slot select); GAP9 side for GAP9 apps |
| [lib/sensei_busarb](lib/sensei_busarb/README.md) | `CONFIG_SENSEI_BUSARB` | One owner at a time for the shared SPI_A bus and the LTE current peaks |
| [lib/sensei_gas_index](lib/sensei_gas_index/README.md) | `CONFIG_SENSEI_GAS_INDEX` | VOC and NOx index from SGP41 raw ticks (Sensirion gas index algorithm inside) |
| [lib/sensei_sensor_math](lib/sensei_sensor_math/README.md) | `CONFIG_SENSEI_SENSOR_MATH` | Unit conversions of the sensor shield drivers (SGP41 ticks, AS7331 uW/cm2, SCD41 ASC hours, LIS2DUXS12 wake threshold) |
| [lib/sensei_pwr_math](lib/sensei_pwr_math/README.md) | `CONFIG_SENSEI_PWR_MATH` | Battery charge estimate per chemistry and button press detection of the power module |
| [lib/sensei_diag](lib/sensei_diag/README.md) | `CONFIG_SENSEI_DIAG_CODEC` | Reset cause bits, fault record and boot state of the diagnostics module (byte layout, names) |
| [lib/sensei_charge](lib/sensei_charge/README.md) | `CONFIG_SENSEI_CHARGE` | Software charge counter: states and events from a table, self-discharge, days left, retained record, swap rules |

## GAP9 bootloader (`GAP9/ssbl`, `GAP9/layouts`)
Optional second stage bootloader for over-the-air updates of the GAP9 firmware: two firmware slots (A and B) in the MRAM, selected by the nRF5340 through one wire (P1.04 to GAP9 pad 56). Apps that keep the SDK bootloader and the default layout are not affected. See [GAP9/ssbl](GAP9/ssbl/README.md) and [GAP9/layouts](GAP9/layouts/README.md).

## Applications (`NRF/apps/`)
- [lpwan_shield](NRF/apps/lpwan_shield/README.md): firmware of the LPWAN shield's nRF9151 (LTE-M, NB-IoT, LoRaWAN over an LR1121). Board `sensei_lpwan_nrf9151/nrf9151/ns`.

## Maintainers
- **Philip Wiese** ([wiesep@iis.ee.ethz.ch](mailto:wiesep@iis.ee.ethz.ch))
- **Sebastian Frey** ([sefrey@iis.ee.ethz.ch](mailto:sefrey@iis.ee.ethz.ch))

## Formatting and Linting

We provide the [pre-commit](https://pre-commit.com) configuration file which you can use to install github hooks that execute the formatting commands on your changes.

You will need to manually install pre-commit since it's not added as a dependency to the `pyproject.toml`:
```bash
pip install pre-commit
```

The configuration sets the default stage for all the hooks to `pre-push` so to install the git hooks run:
```bash
pre-commit install --hook-type pre-push
```
The hooks will run before each push, making sure the pushed code can pass linting checks and not fail the CI on linting.

If you change your mind and don't want the git hooks:
```bash
pre-commit uninstall
```

## License
All licenses used in this repository are listed under the `LICENSES` folder. Unless specified otherwise in the respective file headers, all code checked into this repository is made available under a permissive license.
- Most software sources and tool scripts are licensed under the [Apache 2.0 license](https://opensource.org/licenses/Apache-2.0).
- Some third party files under the `NRF/system/third_party` directory are licensed under the [MIT license](https://opensource.org/license/mit).
- Some third party files under the `NRF/system/third_party` directory are licensed under the [BSD 3 license](https://opensource.org/license/bsd-3-clause).
- Markdown, JSON, text files, pictures, PDFs, are licensed under the [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0) license (CC BY 4.0).

To extract license information for all files, you can use the [reuse tool](https://reuse.software/) and by running `reuse spdx` in the root directory of this repository.
