<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

This example forwards data sensed on the GAP9 to a host over UART.

The GAP9 periodically produces a sensor reading and prints it as a text line.
Its stdout is routed to UART0, which on the SENSEI board is wired to the
nRF5340. The nRF receives the bytes on its `uart_gap` instance (`uart3`) and
forwards them verbatim to **two sinks**: the USB CDC-ACM device (a clean,
data-only port for a host tool) and the SEGGER RTT up-channel (for live viewing
alongside the nRF's own logs). A host sees the GAP9's readings on either.

```
  [sensor]        UART0 (115200, pads 60/61)        uart3                ┌─▶ USB CDC-ACM ─▶ host tool
  GAP9  ──printf──────────────────────────────▶  nRF5340  ──forward──────┤
        "SENSOR,seq=..,value=..\n"                (uart_gap)              └─▶ SEGGER RTT  ─▶ J-Link viewer
```

The inter-SoC link is 115200 baud, configured on **both** ends: `uart_gap` /
`current-speed` in the board DTS, and `CONFIG_IO_UART_BAUDRATE` in the GAP9
`sdk.config`. If you change one, change the other.

> **Troubleshooting:** repeated `GAP UART RX error (reason 0x04)` warnings on
> the nRF mean framing errors, i.e. the GAP9's *actual* bit rate does not match
> the nRF's 115200 (reason `0x04` = framing; `0x01` overrun, `0x02` parity).
> Two GAP9-side settings must both be correct:
>
> 1. `CONFIG_IO_UART_BAUDRATE=115200` in `sdk.config` (the requested baud), and
> 2. the **board clock tree** in `sdk.config` (`CONFIG_FAST_OSC_FREQUENCY`,
>    `CONFIG_FREQUENCY_PERIPH`, ...). The printf UART is initialised before
>    `main()` and its baud divider is derived from the peripheral clock, which
>    depends on the real reference oscillator. If these are left at the generic
>    SDK defaults the divider is computed for the wrong clock and the line runs
>    at the wrong speed, producing a continuous stream of `0x04` errors.
>
> A few transient errors right when the GAP9 boots/reboots are normal; the nRF
> restarts reception automatically.

## Robustness

This example is intentionally written to be robust, addressing the weak spots
of the older `sensei-demo-yolov5` / `sensei-demo-multimodal` forwarders:

- **Continuous, lossless reception.** The nRF uses the asynchronous,
  DMA-backed UART API with a static two-buffer ping-pong, so no bytes are lost
  between buffer swaps. Reception never depends on a terminator character.
- **No dynamic allocation.** RX buffers are static and the RX→worker hand-off
  uses a lock-free `ring_buf`, so there is no per-frame `k_malloc()` and no
  heap fragmentation.
- **Short, non-blocking ISR.** The RX callback only copies into the ring buffer
  and signals a worker thread; all blocking work (writing to the sinks) happens
  in the worker.
- **No stalls on either sink.** The USB write is gated on the CDC-ACM DTR line,
  so a disconnected/unopened port can never back-pressure the pipeline; the RTT
  write is inherently non-blocking (it drops bytes if the viewer is not
  draining the channel).
- **Visible overflow.** If the ring buffer ever overflows, the dropped byte
  count is logged rather than silently lost.
- **Clean USB channel.** The nRF's own logs go to RTT only (see `prj.conf`), so
  the USB CDC-ACM port carries nothing but the forwarded GAP9 data.

## Extending

The GAP9 `read_sensor()` is a placeholder returning a pseudo value; replace it
with a real acquisition (I2C/SPI/ADC/camera). For a dedicated binary protocol
or higher throughput, open UART0 explicitly with `pi_open(PI_UART_UART0, &uart)`
and use `pi_uart_write()` instead of `printf` (see
`sensei-demo-yolov5/src_GAP/src/uart_comm.c`); in that case disable
`CONFIG_IO_TYPE_UART` or serialise printf vs. writes, since both share UART0.

## NRF

Build and flash the nRF application. Its log/console output is on RTT; the
forwarded GAP9 data appears on **both** the USB CDC-ACM serial port and RTT.

```sh
cd src_NRF
west build -b nrf5340_senseiv1/nrf5340/cpuapp -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
west flash
```

Watch the forwarded data on the USB serial port, e.g.:

```sh
# replace with your CDC-ACM device node
picocom -b 115200 /dev/ttyACM0
```

or over RTT (interleaved with the nRF logs):

```sh
JLinkRTTViewer            # or: west rtt / JLinkRTTClient
```

## GAP9

Build and flash the GAP9 application. Once the nRF powers up the GAP9, it boots
this image from flash and starts streaming readings.

```sh
cd src_GAP9
gap init
gap run
```
