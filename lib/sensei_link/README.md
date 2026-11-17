<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_link

The link layer that SENSEI apps use to talk to other chips: framing for byte streams and SPI, fragmentation,
the nanopb glue and the messages of the SDK libraries. C99, no allocation, no OS.

| Part | Files | What |
|---|---|---|
| Stream framing | `cobs.h`, `crc.h`, `stream.h` | `COBS(channel \| message \| CRC-16) 0x00`, a byte-wise receiver |
| SPI frame | `spi.h` | 8 byte header `0x5E, version, type, flags, len, crc16`, then the message. The CRC covers header bytes 0 to 5 and the message |
| Fragments | `frag.h` | message split into radio chunks `msg_id, index, count, data`, with a reassembler |
| nanopb glue | `pb.h` | `sensei_encode_<Name>()` and `sensei_decode_<Name>()`, `SENSEI_PB_DECLARE()` for app messages |
| Messages | `proto/sensei/*.proto` | `types.proto` (Empty, Version, Radio), `lpwan.proto` (nRF5340 to nRF9151 requests, responses, events) |

Apps add their own messages in their own `.proto` files (same package `sensei`, so C and Python names stay
short) and import `sensei/types.proto` and `sensei/lpwan.proto`. The SENSEI IEQ node does that
(`firmware/proto`). The GAP9 and LPWAN libraries (`sensei_gap9`, `sensei_lpwan`) build on this one.

## Use it

- Zephyr: `CONFIG_SENSEI_LINK=y` (needs the SDK as module: `-DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF`).
  The nanopb runtime is the one of the NCS module (`CONFIG_NANOPB`).
- Host and GAP9: `add_subdirectory(${SENSEI_SDK_ROOT}/lib/sensei_link sensei_link)` and link `sensei_link`, or take the
  source lists of `sensei_link.cmake`. GAP9 needs `-std=gnu99` (the generated code uses anonymous unions).
- Host tests: `cmake -S tests -B <builddir>`, or `make test-host` in the SDK root.

## nanopb

The runtime and the generator are one version, **nanopb 1.0.0-dev, commit 5499fd4c** (proto header version 40). It is
the commit that NCS v3.4.1 ships in `modules/lib/nanopb`. On Zephyr the build uses the NCS copy. Host and GAP9
builds use the submodule `third_party/nanopb`, pinned to the same commit. When NCS moves on, move the submodule to the
new NCS commit and run `scripts/generate.sh` (and `firmware/proto/generate.sh` in the node).

## Generated files

`generated/sensei/*.pb.{c,h}` are committed, so GAP9 and host builds need no generator. After a change of a `.proto`:

```sh
python3 -m venv <venv> && <venv>/bin/pip install protobuf grpcio-tools     # once
NANOPB_GENERATOR="<venv>/bin/python third_party/nanopb/generator/nanopb_generator.py" scripts/generate.sh
```

`scripts/generate.sh --check` fails if the files are out of date. An app that has more schemas runs its own generator
with `-I <sdk>/lib/sensei_link/proto` (the node: `firmware/proto/generate.sh`, which also calls this script).

## Rules for changes

- Keep field numbers; add new fields at the end; never change a type; use `reserved` for removed fields.
- Every `repeated`, `bytes` and `string` field needs a size in the `.options` file.
- The framing version is `SENSEI_LINK_VERSION` (2: the SPI CRC covers the header, so a flipped type or flag is caught;
  version 1 covered the payload only). A peer with another version is refused (`SENSEI_SPI_ERR_VERSION`).
- `SENSEI_SPI_MAX_PAYLOAD` (default 8192) is a build option, never a `#define` in one file: Zephyr
  `CONFIG_SENSEI_SPI_MAX_PAYLOAD`, CMake `-DSENSEI_SPI_MAX_PAYLOAD=<n>` (the targets carry it; builds that take the source
  lists add `SENSEI_LINK_DEFINITIONS`). Frames above it are refused. Use the same value for all programs of a link.

## Credits

The message layer follows patchOS (Philipp Schilk, ETH Zurich): one schema per link, size limits in `.options`
files, `oneof` bodies for requests and replies. The first communication subsystem of the SDK (Philipp Schilk and
Philip Wiese, branch `pr/sdk/communication`) and the student project `sensei-demo-communication` (Serge Wüest,
ETH Zurich) were the starting points of the SPI links.
