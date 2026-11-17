# Third-party code

One copy of each vendor library, here or in the `lib/` folder that uses it. The drivers in `NRF/drivers/sensor`
compile what they need; nothing here is built by itself. Each folder keeps the license file of the library.

| Folder | Library | Source and version | License |
|---|---|---|---|
| `sensirion/common` | Sensirion common I2C code | `Sensirion/embedded-i2c-scd4x` 1.2.0 (used by the SCD41 and the SGP41 driver) | BSD-3-Clause |
| `sensirion/scd4x` | SCD4x driver | `Sensirion/embedded-i2c-scd4x` 1.2.0 | BSD-3-Clause |
| `sensirion/sgp41` | SGP41 driver | `Sensirion/embedded-i2c-sgp41` 1.0.0 (built against the 1.2.0 common code) | BSD-3-Clause |
| `bosch/bme68x` | BME68x SensorAPI | `boschsensortec/BME68x_SensorAPI` v4.4.8 | BSD-3-Clause |
| `as7331` | AS7331 register library | SENSEI, written for this SDK | Apache-2.0 |
| `bh1730fvc` | BH1730FVC register library | SENSEI, written for this SDK | Apache-2.0 |
| `st/ilps28qsw` | ILPS28QSW register library | STMicroelectronics `STMems_Standard_C_drivers` | BSD-3-Clause |
| `max77654` | MAX77654 PMIC driver | Philipp Schilk, ETH Zurich | Apache-2.0 |
| `max-m10s/ubxlib` | u-blox ubxlib (git submodule) | `u-blox/ubxlib` | Apache-2.0 |
| `../../../lib/sensei_gas_index/third_party` | Sensirion gas index algorithm (VOC and NOx index) | `Sensirion/gas-index-algorithm` 3.2.2, floating-point version | BSD-3-Clause |

The Sensirion and Bosch files are unchanged copies of the upstream files, without the examples and tests. To
update a library, copy the files of the new release over them and check the drivers in `NRF/drivers/sensor`.
BSEC2 (proprietary) is not part of the SDK.
