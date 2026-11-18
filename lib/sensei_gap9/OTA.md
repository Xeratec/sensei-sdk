<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# GAP9 firmware update over the air

A SENSEI app with a GAP9 updates the GAP9 application without a debug probe. GAP9 holds two firmware slots (A and B) in
its MRAM. A host sends a signed image through the nRF5340. GAP9 writes it into the slot it does not run from, the nRF5340
checks the signature and starts that slot on trial, and the old slot stays until the new one is confirmed.

Untested on hardware. Tested on the host (`make test-host`, `make test-python`) and on GVSoC (the SSBL test in
[`GAP9/ssbl`](../../GAP9/ssbl/README.md) and the A/B test of an app).

| Part | Where | Runs on |
|---|---|---|
| Writer: one transfer into the other slot, checkpoints, checks | `src/g9ota.c`, `include/sensei_gap9/g9ota.h` | GAP9, host tests (no OS, no GAP SDK) |
| MRAM backend, slot detection, readfs of the slot | `gap9/g9ota_mram.c`, `g9ota_mram.h` | GAP9 (GAP SDK) |
| Flow: session, signature, trial boot, revert | `src/g9ota_flow.c`, `g9ota_flow.h` | nRF5340, host tests (no OS) |
| Trial rule | `include/sensei_gap9/gap9_trial.h` | nRF5340, host tests |
| Zephyr glue: settings, signature check, link, timer | `zephyr/gap9_ota.c`, `gap9_ota_zephyr.h` | nRF5340 |
| Messages | `lib/sensei_link/proto/sensei/gap9_ota.proto` | all |
| Image builder | `GAP9/ssbl/tools/pack_slot.py` | host |
| Slot format, layout | `gap9_slot.h`, `GAP9/layouts/sensei_ab.json`, `GAP9/ssbl` | all |

## What the app brings

- **GAP9 app:** the job that carries the six update requests (`G9OTA_OP_*` of `struct g9ota_req`, answered with a
  `Gap9OtaInfo`) in the app's own job message, with
  `g9ota_dispatch()`. The MRAM backend: `g9ota_mram_init(kind, NULL)` at start-up; link with
  `SENSEI_GAP9_OTA_LINK_OPTIONS` (the wrapper of `pi_fs_conf_init`). `kind` is the board id of the images (at most 48
  bytes). `g9ota_init()` refuses a layout whose slots and checkpoint sectors are not sector aligned or overlap, or
  that has no flash operation. The second argument of `g9ota_mram_init()` is a test area for a local test on a
  single-image layout; it is used only in a build with `-DG9OTA_MRAM_TEST_AREA=1` and only when no hand-off record
  exists (see "Which slot starts"), else it is refused.
  The A/B layout of `GAP9/layouts/sensei_ab.json` and the SSBL of `GAP9/ssbl`. See [README.md](README.md).
- **nRF5340 app:** `CONFIG_SENSEI_GAP9_OTA=y` and a `struct sensei_gap9_ota_cfg`: the public key of the signature
  (P-256 point and its SHA-256), a `job` callback that sends a `struct g9ota_req` as a job of the app's format and
  returns the `Gap9OtaInfo` of the answer (errors: `-ENOTSUP` for a GAP9 that cannot be updated, `-EPROTO`, `-EIO` ...),
  an optional `trial_check` (the app's own checks of a new image: self-test, a detect job), and the work queue (needed).
  The flow has no context pointer in its operations: one flow per app. The app
  forwards its host requests to `g9ota_flow_begin/_chunk/_command(sensei_gap9_ota_flow(), ...)` and sends the `Gap9OtaReply` that the flow
  gives to its reply callback (the app copies it into its host message).
- **Host:** an image made by `pack_slot.py --kind <board>`, and the host messages in its own protocol.

The SENSEI IEQ node is the reference: `firmware/gap9/src/ota`, `firmware/nrf5340/src/gap9/gap9_ota*.c`.

## Image

A slot holds an MCUboot-format image: header (0x200 B), a container (the GAP9 app binary and its readfs, which holds the
network weights), a protected TLV `0xa0` with the board id (`kind`), the SHA-256 and an ECDSA P-256 signature. The writer
and the nRF5340 refuse an image of another kind. `pack_slot.py` makes it from `chip.soc.mram.bin` and signs it with
imgtool (`--key`, or `--dev` for MCUboot's public development key: lab nodes only). The version is `--version` or a
`VERSION` file; its major is the job protocol version of the app, so an image with another protocol is not an update.
A downgrade is refused (the same version is allowed). A slot holds 984 KiB.

## Which slot starts, and may I write

The nRF5340 drives the slot line (P1.04, GAP9 pad 56): low A, high B, low whenever GAP9 is off. The SSBL reads the pad,
checks the header, the TLV areas and the section CRCs of that slot and starts the other slot if they fail. The SSBL and
the FSBL are never updated over the air. The nRF5340 owns the decision which slot is good (trial, below).

The pad can be wrong after a fallback. The SSBL writes a hand-off record (`SENSEI_G9_HANDOFF_ADDR` in `gap9_slot.h`:
magic, slot, ~slot, in L2 priv0) before it starts the app. The app takes its slot from the record and from nothing else.
The writer is armed only if there is a record and the slot it names holds a complete image (`g9ota_arm`). Otherwise (the
single-image layout of a development flash, a JTAG load, a reset without the SSBL) nothing is written and the update
jobs answer `NOT_BUILT`. The test area of `g9ota_mram_init()` follows the same rule the other way round: it needs the
build switch and no record, because a record proves that the app runs from a slot of the A/B layout, where the area
could lie on the running slot. The hand-off record must not be covered by a section of the app: `GAP9/ssbl/tools/check_handoff.py` checks the
ELF after the link (GAP9 needs `CONFIG_MALLOC_NO_L2_PRIV=y` and the default linker script).

**Weights.** The readfs is read from the slot the app runs from. The wrapper of `pi_fs_conf_init` sets the volume `app`
and the MRAM offset of the slot's readfs. The stock mount still looks up a partition `readfs_mram` in the volume `app`:
the A/B layout holds a small placeholder of that name, never read.

## Messages

`gap9_ota.proto`. The app puts them into its own messages, with its own field numbers.

| Link | Messages |
|---|---|
| Host to nRF5340 | `Gap9OtaBegin` (size, SHA-256, version, resume), `Gap9OtaChunk` (up to 188 B), `Gap9OtaCommand` (status, finish, apply, abort, revert). Reply: `Gap9OtaReply` (verdict, stage, fail, offset, chunk size, booted and confirmed slot, both slots with version and image id), or an error of the app |
| nRF5340 to GAP9 | `Gap9OtaBegin`, `Gap9OtaData` (up to 1024 B), `Gap9OtaStatus` (with `verify` and a slot it checks that slot like `finish` does, without a transfer), finish, `Gap9OtaSeal` (the image hash that the nRF5340 checked) and abort; answer `Gap9OtaInfo` |

The chunk size 188 B is for a host message of 212 B (a receive buffer of 256 B). The verdict of GAP9 is in
`Gap9OtaInfo.result`; the job status of the app is OK whenever the job was handled.

## Flow

1. `status`: the host reads the slots and versions, refuses a lower version and skips an image that already runs.
2. `begin`: the nRF5340 starts a session. It holds GAP9 powered and SPI_A (`gap9_link_hold`; if the LPWAN client or LTE
   has the bus the request fails with `-EBUSY`). The session ends after `apply`, `abort`, 120 s without a request or 45 min
   after it began. GAP9 opens the transfer into the slot it does not run from and erases its first sector (the old image
   there is gone). A fresh transfer may start only 10 s after the last one (wear; GAP9 forgets its own count when it
   loses power); a `resume` is not fresh.
3. `chunk` at the offset GAP9 wants (every answer carries it). GAP9 collects 8 KiB, erases the MRAM sector, programs and
   reads it back, then appends a checkpoint (offset, size, SHA-256 of the file) to a log in two sectors that take turns.
   The slot that runs is never written: every erase and program goes through an address guard.
4. `finish`: GAP9 reads the slot back and checks, in this order, the SHA-256 of the file, the header and container, the
   kind TLV, the image hash against the bytes, the app descriptor (as the SSBL checks it: no section over the SSBL, the
   hand-off record or the end of L2, no XIP, at most 32 sections), a signature TLV of the right shape and the version.
   It answers VERIFIED with the hash and the signature and records the transfer as checked. A failed check erases the
   first sector. Then the nRF5340 checks the signature over the image hash with its key (`FAIL_SIGNATURE`, detail 1:
   another key, 2: the signature does not check). If it fails, the nRF5340 aborts the transfer on GAP9 (repeated twice
   if the abort gets no answer). If it passes, the nRF5340 sends `seal` with that hash, and only now GAP9 writes the
   first 16 bytes of the file, which hold the MCUboot magic. They are left out while the image comes in (the checkpoint
   records keep them), so a slot that is not sealed never parses and the SSBL never sees it: not half an image, and not
   a complete one that the nRF5340 did not accept (a bad signature, an abort that got lost, a reset of the nRF5340).
   The flow keeps the image id of each slot that it verified or confirmed in the settings, after the seal.
5. `apply` (only for a transfer checked in this session): the nRF5340 stores the trial (settings key
   `sensei_gap9/trial`), answers at once (stage APPLYING), power cycles GAP9 with the slot line at the new slot and
   decides with `sensei_gap9_trial_step()` (`gap9_trial.h`): a HELLO with the right protocol (a boot without one is
   tried twice, a wrong protocol is not), then a status job (booted slot, image id and version are the new ones: a slot
   that did not load would have been replaced by the old one), then the app's `trial_check`. A reboot that finds SPI_A
   busy waits and tries again at the next poll (12 tries). A confirm or revert is stored; if the store fails it is tried
   again every 5 s. Passed: the new slot is confirmed. The host asks `status` until `confirmed_slot` is the new slot or
   the stage is FAILED with `FAIL_BOOT`.
6. Anything else, or a reset of the nRF5340 during the trial: the confirmed slot starts again (`FAIL_BOOT`).
   `revert` is an `apply` of the other slot with the same trial. The flow starts a slot only with an image id it
   verified or confirmed. For a slot with another id (an image from the factory flash, a lost record) GAP9 reads the slot
   back (`Gap9OtaStatus` with `verify`) and the nRF5340 checks the signature as after `finish`; a slot that fails is
   refused (`FAIL_SIGNATURE`). The old image stays in its slot until the next `begin` writes over it.

**Resume.** A request without an answer (link down, GAP9 power cycled) is repeated, then the host asks `status` and
`begin` with `resume`. GAP9 answers the offset of its last checkpoint, a multiple of 8 KiB; a chunk below the wanted offset
is acknowledged and changes nothing. A `begin` with `resume` after a finished transfer (checked, sealed or not) makes
GAP9 check the slot again and answer VERIFIED with the proof (`sealed` says whether the magic is in); the nRF5340 checks
the signature again from the answer and seals, so `apply` works without sending the image again. A transfer that fails
that check is dropped. A power cut at any write, the seal included, leaves either the old state or the new one (the host
tests cut the power at every write of an update, and a second time during the resume).

**Lost settings.** If the nRF5340 has no stored slot (first start, erased settings), it holds GAP9, starts slot A and
confirms the first slot that answers with a HELLO and a status, passes the check of its signature and the app's checks;
B is tried if A does not answer or fails. Neither: slot A stays selected. The probe stops a damaged or unsigned image
from being confirmed, it does not protect against a hostile one that is already running.

## Recovery

Untested on hardware. Off by default: `CONFIG_SENSEI_GAP9_RECOVERY` on the nRF5340 (needs `CONFIG_SENSEI_GAP9_OTA`) and
`SENSEI_SSBL_RECOVERY` in the SSBL build (`GAP9/ssbl`).

If neither slot boots, the SSBL does not stop. It runs the writer of the update itself, over the same SPI link, and the
nRF5340 updates GAP9 through it: no debug probe in the field.

- **HELLO.** The SSBL sends `Version.major` 0xFFFF (`G9L_RECOVERY_MAJOR`), `minor` = the recovery protocol (1), `patch` =
  the reason (1: both slots failed, 2: the strap was high). An nRF5340 without recovery refuses the major (`-EPROTO`) and
  powers GAP9 off: safe. With recovery, the link (`gap9_core.c`) switches to the second codec (`Gap9RecoveryJob`,
  `Gap9RecoveryResult`) and latches recovery. App jobs fail at once with `-ENOEXEC` and GAP9 is not powered for them; the
  link powers it once every `CONFIG_SENSEI_GAP9_RECOVERY_REPROBE_H` hours to look again. A HELLO of the app clears the latch.
  The reply and the Status message report recovery and its reason.
- **Writer.** The one of this document (`g9ota.c`, behind `g9rec.c`), with the slot on the slot line as the booted slot:
  the keep slot. The nRF5340 drives the slot line to its confirmed slot, so keep = confirmed; the flow checks it
  (`FAIL_KEEP`) before a transfer starts, and nothing is written otherwise. The guard never writes the keep slot, the
  boot area or anything outside the other slot and the checkpoint sectors. The SSBL checks no signature: the nRF5340
  does and then seals, as in the normal update.
- **Flow.** The same requests: `begin`, `chunk`, `finish`, `apply`. The flow sends them as recovery jobs when the link
  says recovery, and sends the first job again as a recovery job if it met the boot loader before the latch was set.
  `apply` power cycles GAP9 with the slot line at the new slot; the SSBL starts it (the keep slot is bad), the app
  arms the writer, which drops the record of the transfer, the trial confirms it and the latch goes. A trial or the
  lost-settings probe that lands in recovery is a failed boot, and the flow never writes by itself.
- **Rollback.** The flow keeps the highest version it confirmed (`min_version`, in the settings) and refuses an older
  image (`FAIL_ROLLBACK`) before it seals, in recovery and in the normal mode: in recovery the keep slot cannot tell what
  was running. A revert to the old slot is not a transfer and is not checked.
- **Rate limits.** The rules of the normal update hold: a new transfer at least 10 s after the last, a session ends after
  120 s without a request or 45 min, a forced recovery once per session.
- **Strap.** `Gap9OtaBegin.force_recovery` powers GAP9 with P1.06 (pad 57) high, for a GAP9 that boots an image but hangs
  before its HELLO. Only with `CONFIG_SENSEI_GAP9_RECOVERY_STRAP`, and not while a transfer is open. That P1.06 is a free
  direct wire to pad 57 is UNVERIFIED (`TESTING.md`, T18.18): off by default.

Seal and power: after `seal` the new slot is the only one that boots. A power cycle of GAP9 between `seal` and `apply`
starts it as the fallback, without a trial; send `apply` right after the check.

## Keys

The signature is checked by the nRF5340, not by GAP9 or the SSBL. The public key is in the nRF5340 image (the app takes it
at build time) and cannot be changed over the air. Use a key that differs from the keys of the nRF5340 image and of
other boards.

## Compatibility

Deferred sealing changed the job protocol: the nRF5340 sends the `seal` job, and the GAP9 writer no longer writes the
magic in `finish`. An app takes the change with a new major version of its job protocol (the SENSEI IEQ node: 2): a new
nRF5340 refuses an old GAP9 application (HELLO with the old major, `-EPROTO`, GAP9 powered off) and an old nRF5340 refuses
a new one, so the two never talk with different rules. An old writer sealed in `finish`, before the nRF5340 checked the
signature; it must not stay in the field. A node that has such an image takes the new one over JTAG once.

## Not covered

The SSBL and FSBL (JTAG only), a GAP9 firmware with another job protocol major (it needs a new nRF5340 image first), no
authentication of the ROM boot, a node whose nRF5340 settings are lost while a trial is open may keep an image that
was not confirmed (the probe confirms what answers). A board that has no A/B layout needs one JTAG flash first.

## Tests

`tests/test_g9rec.c` (the recovery job server on a RAM MRAM with both slots bad: transfer, seal, keep slot never written,
power cut at every write), `tests/test_g9ota.c` (the writer on a RAM MRAM: transfer, deferred seal, resume, a power cut at every write, the seal
included, the running slot never written, the checks of `finish`, the address guard), `tests/test_g9ota_flow.c` (the flow against a fake link whose GAP9
runs the real writer: transfer, signature, trial, revert, reset in a trial, lost settings), `tests/test_gap9_trial.c`,
`GAP9/ssbl/tools/test_pack_slot.py` (the builder, and an imgtool image through the writer; CI fails if these skip).
