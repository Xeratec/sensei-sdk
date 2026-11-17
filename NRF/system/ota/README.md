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
| Trial boot | after an update the image runs on trial: `sensei_ota_init()` confirms it when the app's check passes and an SMP request came in on the image, and resets the node if that does not happen in time, so MCUboot reverts | `SENSEI_OTA_CONFIRM`, `_CONFIRM_NEEDS_LINK`, `_CONFIRM_DELAY_S`, `_TRIAL_TIMEOUT_S` |

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
reachable. The option selects `MCUMGR_SMP_COMMAND_STATUS_HOOKS`, which calls the hook, and a `BUILD_ASSERT` fails
the build if that option is off. A frame of the app's own protocol is no proof: it works with a broken SMP path.
Debug and Release can use the same setting; turn it off only when the host never sends SMP after an update.
`sensei_ota_link_seen()` is for an update path other than SMP. The host tool must send one SMP request (for example
the image list) after the reboot, or the image reverts. The decision is the pure function in
[`sensei_ota_trial.h`](sensei_ota_trial.h); its host test is in `tests/` (`make test-host` of the SDK).

The app sets the access to the Bluetooth service with the Zephyr choice `MCUMGR_TRANSPORT_BT_PERM_*` (open by
default; `RW_ENCRYPT` needs `BT_SMP`). The mcumgr groups beyond image and OS reset (echo, info, ...) are the app's
choice.

Shared stream: call `sensei_smp_stream_init(tx, user)` once, then pass every received byte to
`sensei_smp_stream_feed()`. It returns the bytes that belong to the other protocol. The other protocol's frames must
end with 0x00 and must not start with `0x06 0x09` or `0x04 0x14`. The node app does this for its USB host link
(`firmware/nrf5340/src/comm/host_link.c`).

## Keys and sysbuild

Signing keys and the build type belong to the app's sysbuild files, not to this module. The node's setup is in
`firmware/nrf5340/{sysbuild.conf,sysbuild.cmake,Kconfig.sysbuild}`.

## Not covered

- The network core of the nRF5340: the NCS update path needs the b0n bootloader and the partition manager.
- Images for other processors (nRF9151, GAP9): the app receives them over SMP and writes them itself. This module
  only keeps the SMP server and the trial logic general.
