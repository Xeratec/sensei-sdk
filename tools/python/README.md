<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_sdk (Python)

The generic host side of the SDK, for the tools of every SENSEI app. Each module mirrors a C library, and a test keeps the
two in line.

| Module | Mirrors | Content |
|---|---|---|
| `framing` | `lib/sensei_link` | CRC-16/32, COBS, stream frames (`StreamDecoder`), SPI frames, fragments and `Reassembler`, the channel ids and limits of `sensei_link.h` |
| `secure` | `lib/sensei_secure` | key store (sender id, counters), P-256 key derivation, envelope version 3 (`seal_raw`, `open_sealed`), replay window; sealed settings records of the node (`seal_record`, `open_record`); key store file sealed with a 32 byte key file (`KeyStore(key_file=...)`, `SENSEI_KEYSTORE_KEY_FILE`); `python -m sensei_sdk.secure new-sender | new-key-file | seal-file | unseal-file | rotate-file-key` |
| `diag` | `lib/sensei_diag` | reset cause and reason names, the 52 B fault record (`pack_fault`, `unpack_fault`), `addr2line` and the ELF lookup by image id. The job names of `last_job` belong to the app: pass `job_names` |
| `smp`, `smp_sim` | `NRF/system/ota` | MCUboot SMP client (serial and BLE links; upload, test, confirm, reset) and a simulated server. The board id of an image is a parameter: `load_image(source, kind=...)`; `python -m sensei_sdk.smp` is a small command line for it |
| `transport`, `transport_serial`, `transport_ble` | | byte transports (loopback, USB serial, BLE). The GATT UUIDs and the advertising name are a `BleProfile` that the app supplies |
| `vectors` | | the C test vectors of the three libraries above (below) |

Needs Python 3.11 and `cryptography`. The serial and BLE transports need the extras `serial` (pyserial) and `ble` (bleak).

## Use in an app

```sh
uv add "sensei-sdk @ git+https://github.com/Xeratec/sensei-sdk@<commit>#subdirectory=tools/python"
```

Pin the commit that the app's west manifest uses for the SDK. To work against a local checkout, install it
editable over that (`uv pip install -e <sdk>/tools/python`) and leave the pin as it is.

```python
from sensei_sdk import framing, secure
from sensei_sdk.transport_ble import BleProfile, BleTransport

PROFILE = BleProfile(service="...", adv_name="MYDEV", char_rx="...", char_tx="...")
transport = BleTransport(address, PROFILE)
```

## Tests and vectors

```sh
python -m pytest tools/python/tests        # or: make test-python
python -m sensei_sdk.vectors               # write the C test vectors
python -m sensei_sdk.vectors --check       # fail if they are out of date (CI)
```

The vectors go to `lib/sensei_link/tests/generated/vectors.h`, `lib/sensei_secure/tests/generated/vectors.h` and
`lib/sensei_diag/tests/generated/vectors.h`; the host tests of the libraries check the C code against them
(`make test-host`). `tests/test_sdk_constants.py` compares the Python constants with the C headers. An app with
messages of its own builds its vectors on `vectors.pattern`, `array`, `pairs` and `frag_cases`.
