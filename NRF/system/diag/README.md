<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Field diagnostics

What a node in the field can tell after a reset: why it reset, how often, and what it did when it crashed.
Enable with `CONFIG_SENSEI_DIAG=y` (needs no board of the SDK, only the retained RAM below for a safe record). The
record layout, the reset cause bits and their tests are in [lib/sensei_diag](../../../lib/sensei_diag/README.md).

- **Reset cause.** At boot (`SYS_INIT`, POST_KERNEL) the module reads the hwinfo reset cause, clears it and keeps it:
  `sensei_diag_reset_cause()`. Use it instead of reading hwinfo, which can be read once.
- **Boot counter.** Kept in the retained RAM: boots since it was last lost (power-up, brown-out, battery change).
  `sensei_diag_boot_count()`.
- **Fatal error record.** The module defines `k_sys_fatal_error_handler()`. It writes the reason (`K_ERR_*`, on Cortex-M
  the fault class such as `usage-undefined-instruction`), PC, LR, the stacked xPSR, the thread name (`ISR` in an
  interrupt), the uptime, the id of the running image, the boot count and the **last job id** into the retained RAM, then
  does what `RESET_ON_FATAL_ERROR` of NCS does (log, reset). Both define the same function: turn
  `RESET_ON_FATAL_ERROR` off (the module's `SENSEI_DIAG_RESET_ON_FATAL` replaces it, default on; off: halt). An
  application with its own handler sets `SENSEI_DIAG_FATAL_HANDLER=n` and calls `sensei_diag_capture()` first.
- **Lockup.** A fault inside a fault handler, for example an overflow of the interrupt stack, locks the core up. The chip
  resets (nRF) with `CPU_LOCKUP` and no record. The reset cause mapping adds `FAULT` for it, so it is counted as a fatal
  error (`errors.reset`, Status); `sensei_diag_take_fault()` finds no record. QEMU stops on a lockup instead of resetting.
- **In the handler.** The handler takes no lock: the record is written first (uptime 0) and the uptime is added by a second write,
  because `k_uptime_get()` takes the timer lock, which a fault in the timer code can hold (an assert with `SPIN_VALIDATE`). A fault
  inside the second write does not start another capture, so the record of the first fault stays. The retention and
  retained-memory mutexes must be off (the build checks it; `SENSEI_DIAG` implies the options).
- **Once.** The next boot moves the record out of the retained RAM; `sensei_diag_take_fault()` hands it over once (`_peek_`
  does not clear). The application decides what to keep: the node stores the bytes of `sensei_diag_fault_pack()` in its
  settings and puts the record in `Status`.
- **Job id.** `sensei_diag_set_job(id)` is one store: call it where a job starts (the node's scheduler does). The record
  shows the job that ran last. `sensei_diag_set_image_id(id)` names the image (the node uses the first 4 bytes of the
  image hash), so a tool finds the ELF for `addr2line`.
- **Other cores.** `sensei_diag_capture_ext()` writes a record for a fault that Zephyr does not see. The LPWAN shield uses it
  for the modem core (reason `SENSEI_FAULT_MODEM`, `nrf_modem_fault_handler`).

## Retained RAM

Devicetree: three `zephyr,retention` areas in a `zephyr,retained-ram` region, chosen as `sensei,diag-fault` (64 B),
`sensei,diag-state` (16 B) and `sensei,diag-dump` (896 B, only for the coredump). The SDK boards have them
(`nrf5340_senseiv1_retained.dtsi`, `sensei_lpwan_nrf9151_nrf9151_common.dtsi`): the last 1 kB of the application RAM, taken out
of the image RAM, so the linker, the heap and MCUboot do not use it. Other boards or tests add the same nodes (see
`NRF/tests/diag_fault/app.overlay`). The areas have no checksum of the retention subsystem: the record has its own CRC-32.
Without them the record lives in `noinit` RAM, which usually survives a warm reset but is not guaranteed. The module wants
no mutex in the retention drivers (`imply RETENTION_MUTEX_FORCE_DISABLE`), the handler writes from an interrupt.

**MCUboot.** The same board file is used for MCUboot (`..._cpuapp_bootloader.dts`): it does not touch the region. A reset
that MCUboot passes keeps the RAM; a power cut does not (the record and the count are gone then, the reset cause says POR).

## Coredump (optional)

`CONFIG_SENSEI_DIAG_COREDUMP=y` (also `DEBUG_COREDUMP_BACKEND_OTHER=y`, `DEBUG_COREDUMP_MEMORY_DUMP_MIN=y` and
`DEBUG_COREDUMP_THREAD_STACK_TOP_LIMIT_FOR_CURRENT=<bytes>`) keeps the Zephyr coredump of the faulting thread in the
`sensei,diag-dump` area: registers and the top of the stack, as much as fits in 896 B (the rest is dropped).
`sensei_diag_dump_read()` copies it out; the record has the flag `SENSEI_FAULT_FLAG_COREDUMP`. Cost: about 1.2 kB of flash (Debug build of the node) and no
extra RAM (the 1 kB region is reserved either way). The dump is truncated, so Zephyr's `coredump_gdbserver.py` cannot read it
whole; the registers at its start (after the 12 B header and the arch block header) are enough to rebuild the faulting frame.
Keep it off in Release unless you need it.

## Tests

- `lib/sensei_diag/tests`: host test of the codec and the reset cause mapping (`make test-host`).
- `NRF/tests/diag` (`sensei.diag`, native_sim): boot counter, reset cause, the record written by `sensei_diag_capture()` and taken
  after the next boot, damaged and foreign RAM contents. native_sim has no retained RAM, so the test brings a RAM-array driver.
- `NRF/tests/diag_fault` (`sensei.diag.fault`, qemu_cortex_m3): a thread calls `k_oops()`, then executes an undefined instruction;
  the node resets each time and the test checks the record (reason, thread, job, image id, PC, boot count), that it is read once,
  and the coredump, across three boots. Runs in the Docker image of `scripts/twister.sh`.
- `NRF/tests/diag_isr` (`sensei.diag.fault.isr`, `mps2/an521/cpu0`, a Cortex-M33 with stack guards): a fault in an interrupt
  (thread `ISR`, exception number in the xPSR), a thread stack overflow, `k_panic()` in an interrupt, across four boots.
  `sensei.diag.fault.isr_stack` overflows the interrupt stack: the core locks up and QEMU stops, so the test only checks that the
  case runs to the overflow; the accounting of `CPU_LOCKUP` is in the host test of `lib/sensei_diag`.

Untested on hardware: RESETREAS after MCUboot, the retained RAM over a real reset and over MCUboot's swap, the nRF9151 with TF-M
(reading the reset reason from the non-secure image).
