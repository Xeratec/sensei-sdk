<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# SENSEI OTA helpers

Firmware update for apps that MCUboot starts (sysbuild with `SB_CONFIG_BOOTLOADER_MCUBOOT=y`). Enable it with
`CONFIG_SENSEI_OTA=y`.

| Part | What it does | Kconfig |
|---|---|---|
| mcumgr | selects the image and OS groups, the flash helpers and `REBOOT` | `SENSEI_OTA` |
| SMP over Bluetooth | the standard GATT SMP service | `SENSEI_OTA_SMP_BT` |
| SMP on a shared byte stream | mcumgr serial lines on a stream that also carries 0x00-delimited frames of another protocol (USB CDC) | `SENSEI_OTA_SMP_STREAM` |
| Recovery reset | the SMP reset command with a boot mode other than 0 (reset into the MCUboot serial recovery) is refused unless USB power is present; a normal reset always works | `SENSEI_OTA_RECOVERY_NEEDS_USB` |
| Trial boot | after an update the image runs on trial: `sensei_ota_init()` confirms it when the app's check passes and an SMP request came in on the image (hook registered at boot, `SYS_INIT`), and resets the node if that does not happen in time, so MCUboot reverts | `SENSEI_OTA_CONFIRM`, `_CONFIRM_NEEDS_LINK`, `_CONFIRM_DELAY_S`, `_TRIAL_TIMEOUT_S` |

API: [`sensei_ota.h`](sensei_ota.h).

## Use

```
CONFIG_SENSEI=y
CONFIG_SENSEI_OTA=y
CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=512   # one SMP packet at the largest ATT MTU
```

```c
#include "ota/sensei_ota.h"

static bool healthy(void) { return my_app_is_fine(); }   /* must not block */

sensei_ota_init(healthy);   /* at the end of start-up */
```

Why the link counts: a new image that works but has a broken Bluetooth or USB link would otherwise confirm itself
and could not be updated again. With `SENSEI_OTA_CONFIRM_NEEDS_LINK` (default on) it stays on trial until an SMP
request came in on it (any mcumgr command, on BLE or USB), so MCUboot reverts it if the update path was never
reachable. The option selects `MCUMGR_SMP_COMMAND_STATUS_HOOKS`, which calls the hook (registered at boot by
`SYS_INIT`, so a request during start-up counts too), and a `BUILD_ASSERT` fails the build if that option is off. A
frame of the app's own protocol is no proof: it works with a broken SMP path.
Debug and Release can use the same setting; turn it off only when the host never sends SMP after an update.
`sensei_ota_link_seen()` is for an update path other than SMP. The host tool must send one SMP request (for example
the image list) after the reboot, or the image reverts. The decision is the pure function in
[`sensei_ota_trial.h`](sensei_ota_trial.h); its host test is in `tests/` (`make test-host` of the SDK).

The app sets the access to the Bluetooth service with the Zephyr choice `MCUMGR_TRANSPORT_BT_PERM_*` (open by
default; `RW_ENCRYPT` needs `BT_SMP`). The mcumgr groups beyond image and OS reset (echo, info, ...) are the app's
choice.

**Recovery reset.** With `MCUMGR_GRP_OS_RESET_BOOT_MODE` (retention boot mode) the SMP reset command takes a `boot_mode`;
boot mode 1 makes MCUboot start its serial recovery on USB, where it waits without a time-out and the node is off the air.
Anyone who can send SMP (any Bluetooth device in Debug, any bonded one in Release) could park a node that way.
`SENSEI_OTA_RECOVERY_NEEDS_USB` (default on, selects `MCUMGR_GRP_OS_RESET_HOOK`) registers a hook for
`MGMT_EVT_OP_OS_MGMT_RESET` that answers `MGMT_ERR_EACCESSDENIED` to a boot mode other than 0 unless USB power is present
(`sensei_ota_usb_power()`: `VBUSDETECT` of the nRF5340, weak, an app may define its own). The hook sees the request, not the
transport, so VBUS is the test: whoever has the cable plugged in can ask. A reset into the recovery over Bluetooth from a node
on battery is refused, a normal reset and any reset over USB work. The rule is the pure function in
[`sensei_ota_reset.h`](sensei_ota_reset.h), with a host test in `tests/`. Untested on hardware: VBUSDETECT on the SENSEI board.

Shared stream: call `sensei_smp_stream_init(tx, user)` once, then pass every received byte to
`sensei_smp_stream_feed()`. It returns the bytes that belong to the other protocol. The other protocol's frames must
end with 0x00 and must not start with `0x06 0x09` or `0x04 0x14`. The node app does this for its USB host link
(`firmware/nrf5340/src/comm/host_link.c`).

## Keys and sysbuild

Signing keys and the build type belong to the app's sysbuild files, not to this module. The node's setup is in
`firmware/nrf5340/{sysbuild.conf,sysbuild.cmake,Kconfig.sysbuild}`.

## Image id and board TLV

`sensei_ota_image.h` (pure, host-tested in `tests/`) finds a TLV in the trailer of a signed MCUboot image through a read
callback: the image id (first 4 bytes of the SHA-256 TLV, `sensei_ota_image_id()` of the node and of the LPWAN shield app) and
the board TLV `0xa0`, a protected TLV (covered by the signature) that names the board an image is for. An app adds it in its
`sysbuild.cmake`: `set_config_string(${DEFAULT_IMAGE} CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS "--custom-tlv 0xa0 <board>")`. The shield
app of this SDK checks it before it accepts an image; the host tools of SENSEI IEQ check it for both boards. MCUboot itself
ignores it.

## Tests

- Host (`tests/`, `make test-host`): the trial decision, the image TLVs, the recovery reset rule.
- Zephyr, native_sim (`NRF/tests/ota_reset`, `NRF/tests/ota`, see `NRF/tests/README.md`): the reset hook in the mcumgr
  callback chain; the trial supervision (check after the delay, SMP request needed to confirm, retry of a failed
  confirm write, reset at the end of the trial, confirm by the host) with MCUboot's image state and the reset as fakes.

## Not covered

- The MCUboot swap and the trailer flags: the tests fake `boot_is_img_confirmed()` and `boot_write_img_confirmed()`.
- The network core of the nRF5340: the NCS update path needs the b0n bootloader and the partition manager.
- Images for other processors: the nRF9151 of the LPWAN shield has its own MCUboot and is updated through the client of
  `lib/sensei_lpwan` (`sensei_lpwan_dfu_submit()`); the shield app uses the pure trial rule `sensei_ota_trial.h` of this folder.
  GAP9 is not covered.
