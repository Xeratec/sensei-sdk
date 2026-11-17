# BSEC2 (not supported)

BSEC2 is Bosch's proprietary IAQ library. It is not in this repository and the BME688 driver does not call it.
The driver is the plain Bosch BME68x SensorAPI in forced mode with one heater step, which gives temperature,
humidity, pressure and gas resistance.

BSEC2 sets the sensor's heater profile and sample times itself and needs its own scheduler loop, so it does not fit the
start/fetch driver. If you need it: accept the license, download BSEC2 from Bosch Sensortec, and write a second driver
variant that feeds `bsec_sensor_control()` and `bsec_do_steps()` from the same `bme68x` calls. Keep the library files out
of git.
