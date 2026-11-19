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
| `framing` | `lib/sensei_link` | CRC-16/32, COBS, stream frames (`StreamDecoder`), SPI frames, fragments and `Reassembler`, the channel ids and limits of `sensei_link.h`; `looks_sealed` and `kind_of` (the kind of a sealed payload without the generation bits) |
| `secure` | `lib/sensei_secure` | key store (file version 3: sender id, counters, forward secrecy state), P-256 key derivation, envelope version 3 (`seal_raw`, `open_sealed`, `open_sealed_ex`, `peek_sealed`, `seal_downlink`), replay window; the node twin `NodeSession` (also the ratchet side of a node: `rekey_request`, `rekey_answer`); sealed settings records of the node (`seal_record`, `open_record`); key store file sealed with a 32 byte key file (`KeyStore(key_file=...)`, `SENSEI_KEYSTORE_KEY_FILE`; a key file that the group or others can access is refused, `new_key_file` syncs the file and its folder); `python -m sensei_sdk.secure new-sender \| new-key-file \| seal-file \| unseal-file \| rotate-file-key` |
| `ratchet` | `lib/sensei_secure` | forward secrecy primitives: root key R_0, X25519, the mix, the uplink chain (`Chain`), epoch math. The envelope bits of generation 1 and later are in `secure` (`kind_of`, `seal_raw(gen=...)`, `Header.ratchet`, `gen_bits`) |
| `chainwalk` | | cost control for the uplink chain walk of the key store: time-bound gap, cache of walked keys, budgets (see "Forward secrecy in the key store") |
| `errors` | | the exceptions of the envelope (`SecureError` and its subclasses; `secure` re-exports them) |
| `diag` | `lib/sensei_diag` | reset cause and reason names, the 52 B fault record (`pack_fault`, `unpack_fault`), `addr2line` and the ELF lookup by image id. The job names of `last_job` belong to the app: pass `job_names` |
| `smp`, `smp_sim` | `NRF/system/ota` | MCUboot SMP client (serial and BLE links; upload, test, confirm, reset) and a simulated server. The board id of an image is a parameter: `load_image(source, kind=...)`; `python -m sensei_sdk.smp` is a small command line for it |
| `transport`, `transport_serial`, `transport_ble` | | byte transports (loopback, USB serial, BLE). The GATT UUIDs and the advertising name are a `BleProfile` that the app supplies |
| `vectors` | | the C test vectors of the three libraries above (below), including the ratchet vectors of `sensei_secure` |

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

## Forward secrecy in the key store

The key store file is version 3. A node starts in generation 0 (the static keys); every few days it asks for a mix, the
collector answers, and from then on uplinks and downlinks use new keys that an old file cannot derive. Version 2 files
migrate by themselves at the first load (R_0 comes from the static keys); an old reader refuses version 3.

```python
store = KeyStore.load(path, private_pem=pem, key_file=key_file, responder=True)   # responder: SENSEI_REKEY_RESPONDER=1
o = secure.open_sealed_ex(frame, store)       # Opened(node_id, kind, plaintext, gen, promoted)
req = RekeyRequest.decode(field14_bytes)      # taken from the Uplink after it was opened
ans = store.rekey_answer(o.node_id, req)      # RekeyAnswer or None; put ans.encode_field() into a sealed Downlink
frame = secure.seal_downlink(o.node_id, secure.KIND_DOWNLINK, payload, store)   # key of the generation in use
store.node_state(o.node_id)                   # gen, answered mix waiting, previous chain, last verified, highest counter
```

- Open the uplink first, then answer its request: the first uplink of a new generation promotes the answered mix, and a
  request for the generation after it can ride in that same uplink. A request is matched on (gen, up_base, node key), so the
  repeat after a reboot gets the same answer. The answer is on disk (fsync) before `rekey_answer` returns.
- `responder=False` (the default; `SENSEI_REKEY_RESPONDER=1` turns it on) opens messages but never answers: the first
  rollout step. A GUI with its own file does not answer either; give it `follow=<file of the collector>` and it takes over
  the current generation, the answered mix and the previous chain (never the root key, senders, counters). Without it a
  key store loses access to a node at the next mix.
- `add_node` starts the node in generation 0 again and keeps the counters (what a node does when it is provisioned).
  A node that is ahead of the key store ("node ahead of key store: re-provision" in the log, `GenerationError`) needs
  USB provisioning.
- Walking the chain for a far message costs one HMAC per epoch, and anyone can send a frame with a far counter. The
  limits (`chainwalk`): a message may be ahead by at most `min(SENSEI_E2E_MAX_SKIP, 4096 + 1 epoch per second since the
  last verified one)` epochs, else `GapError` at once; walked keys are cached at checkpoints every 1024 epochs (at most
  32 kB per node, LRU over all); budgets of 2^16 steps per node and 2^19 for the store per minute (`WalkBudgetError`,
  the walk goes on at the next frame); the walk runs outside the store lock. Logs about refused frames are limited to one
  per node and minute; `store.stats` counts them.

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
