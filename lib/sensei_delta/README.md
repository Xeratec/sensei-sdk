# sensei_delta

Lossy delta coder for windows of seven sensor channels, in C99. Closed-loop DPCM with a fixed quantizer
step, then static rANS or canonical Huffman coding. No dynamic allocation, caller-provided buffers, only
`<stdint.h>`, `<stddef.h>`, `<string.h>` and `floor()` from `<math.h>`. Build with `-ffp-contract=off`.

The engine is generic. The trained part (symbol frequencies, Huffman lengths, scaler, quantizer steps,
prefilter coefficients) comes from the application as a `struct sensei_delta_tables`. The tables of the SENSEI
IEQ node are generated from its training data and live in that repository (`firmware/lib/sensei_delta_ieq`).

## Format (version 1)

- 7 channels, windows of 1 to 512 samples. Input values are 16-bit "codec units"
  (`u = floor((x - lo) * k + 0.5) + 16384`, clamped; `sensei_delta_to_units()`).
- Optional prefilter: first-order low-pass per channel in Q15, state runs on across windows.
- DPCM per channel: `d = u[t] - prev`, `q = sign(d) * ((|d| + s/2) / s)`. `|q| <= 64` gives symbol `q + 64`
  and `prev += q * s`. Otherwise the escape symbol 129 and the raw 16-bit `u[t]` follow, `prev = u[t]`.
  The error is at most `s/2`.
- Payload: the 7 first samples (u16, little endian, 14 B), then the entropy-coded symbols channel by channel.
  rANS: 32-bit state, `L = 2^23`, 15-bit frequencies, 4 B flush. Huffman: canonical, at most 15 bits, MSB first.
- Same input and tables give the same bytes on every target. The node repository has the full bit-exact
  specification (`networks/delta/FORMAT.md`) and a Python reference.

## API (`include/sensei_delta.h`)

- `struct sensei_delta_tables {n_sets, n_levels, level_step, lo, k, prefilter_a, sets}`: the trained data. A
  `struct sensei_delta_set {freq, len}` holds `[level][channel][symbol]` arrays; either can be NULL.
- `sensei_delta_to_units(tab, ch, x)`, `sensei_delta_from_units(tab, ch, u)`: value to codec units and back.
- `struct sensei_delta_prefilter`, `sensei_delta_prefilter_reset(f)`, `sensei_delta_prefilter_step(tab, f, in, out)`.
- `sensei_delta_encode(tab, cfg, u, n, out, out_cap, &out_len, scratch, scratch_len)`: `u` is `[7][n]` u16,
  `cfg = {set, level, entropy}` with `SENSEI_DELTA_ENTROPY_RANS` or `_HUFFMAN`. `scratch` needs
  `SENSEI_DELTA_SCRATCH_SIZE(n)` bytes, `out` up to `SENSEI_DELTA_MAX_PAYLOAD(n)`. Returns 0 or a negative
  `SENSEI_DELTA_E_*` code.
- `sensei_delta_decode(tab, cfg, in, in_len, n, uhat)`: `uhat` is `[7][n]` int32. Fails with
  `SENSEI_DELTA_E_CORRUPT` unless the payload is consumed exactly.

`SENSEI_DELTA_ENABLE_RANS` and `SENSEI_DELTA_ENABLE_HUFFMAN` (default 1) compile a coder out; the application
can use the same macro to leave its Huffman length tables out.

## Use

**Zephyr** (nRF): `CONFIG_SENSEI_DELTA=y` with `CONFIG_SENSEI_DELTA_RANS`, `_HUFFMAN` and
`CONFIG_SENSEI_DELTA_PROFILE` (DWT cycle hooks). Add the tables to the application sources.

**Host or plain CMake**: `add_subdirectory(<sdk>/lib/sensei_delta sensei_delta)` and link `sensei_delta`.
Options: `SENSEI_DELTA_HUFFMAN`, `SENSEI_DELTA_RANS`, `SENSEI_DELTA_PORT` (`NRF5340`, `GAP9`, `HOST`).

**GAP9 SDK project**: the GAP SDK sets include paths and flags per target, so the engine is compiled into the
application target:

```cmake
include(<sdk>/lib/sensei_delta/sensei_delta.cmake)
set(SENSEI_DELTA_PORT GAP9)      # optional, pi_perf hooks
sensei_delta_add_sources(${TARGET_NAME})
```

## Profiling hooks

`SENSEI_DELTA_PROF_BEGIN(tag)` and `SENSEI_DELTA_PROF_END(tag)` wrap the DPCM (`dpcm`) and entropy-coder
(`coder`) parts of the encoder. They do nothing by default. With a port (`SENSEI_DELTA_PORT_NRF5340`, `_GAP9`,
`_HOST`) `include/sensei_delta_port.h` accumulates cycles in `sensei_delta_prof_cycles[]`; call
`sensei_delta_port_init()` once. Define `SENSEI_DELTA_MARK_HIGH()` and `SENSEI_DELTA_MARK_LOW()` before including
it to drive a marker GPIO.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_delta/tests -B build && cmake --build build &&
ctest --test-dir build`. `test_roundtrip` runs random windows, escapes, N = 1, 2, 3, 512, both coders, error
paths and prefilter properties against synthetic tables (`tests/generated/`, made by `tests/gen_standin.py`).
The bit-exact vectors of a trained table set belong to the application that owns the tables.
