
## NRF

Make sure your `SENSEI_SDK_ROOT` environment variable is set to the path of the SENSEI SDK.

The example builds MCUboot together with the application (sysbuild, `sysbuild.conf`), plus the network core image.
MCUboot is configured in `src_NRF/sysbuild/mcuboot.conf` (USB serial recovery). Build and flash all of it with:

```bash
$> cd src_NRF
$> west build -b nrf5340_senseiv1/nrf5340/cpuapp -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
$> west flash
```

The partitions are the devicetree partitions of the board (MCUboot 64 kB, two image slots of 448 kB).
MCUboot fits its partition with 44 B to spare: keep its log level low.

Next, you can flash the application via the serial recovery bootloader over USB by running:
```bash
$> mcumgr-client upload build/src_NRF/zephyr/zephyr.signed.bin
```

Finally, flash the application via the bootloader the device by running:
```bash
$> mcumgr --conntype ble --connstring 'peer_name=mcuboot' upload build/src_NRF/zephyr/zephyr.signed.bin
```

Alternatively you can use [mcumgr-web](https://github.com/boogie/mcumgr-web/tree/main) to upload, test and then confirm the update.

1. Open mcumgr-web in a browser (Online Version: https://boogie.github.io/mcumgr-web/)
2. Connect to the device
3. Upload the application binary
4. Enable test mode for the bi
5. Reboot the device
6. Confirm the update
7. Optionally erase the old application

I recommend using the  Chrome browser.

## GAP9