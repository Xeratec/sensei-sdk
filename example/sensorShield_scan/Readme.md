This example demonstrates how to interface the sensor shield and scan the sensors connected to it.

## NRF

Scans I2C A and I2C B every 3 s and prints the devices found. The SCD41, SGP41, AS7331 and BH1730 sit behind
power or isolation switches, so the example powers them through their SDK drivers (`pm_device_runtime_get`)
for the I2C B scan and switches them off afterwards.

## GAP9
