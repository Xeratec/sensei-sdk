# Sensor drivers

- `ilps28qsw`: the ILPS28QSW pressure sensor (node `st,ilps28qsw`, sensor shield, I2C_B 0x5C). Zephyr 4.4 has no driver for it. One-shot mode on the SDK register library, so it needs `CONFIG_SENSEI=y`. Untested on hardware.
- `lis2duxs12_extra`: wake-up (motion) interrupt on INT1 and deep power-down for the LIS2DUXS12, which the Zephyr driver does not offer (`<sensei/drivers/lis2duxs12_extra.h>`). Use it next to the Zephyr node. Untested on hardware.

The LIS2DUXS12 and the ISM330DHCX use the Zephyr drivers, which need the `hal_st` module in the west workspace.
