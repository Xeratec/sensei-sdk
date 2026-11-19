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
| `commission` | `lib/sensei_secure` (later) | adding a node over BLE with a setup code: SPAKE2+ (RFC 9383) as `Prover` and `Verifier`, scrypt registration (`make_verifier`), the kind 5 `Channel`, fingerprints and proof of possession, setup code with Verhoeff check digit, QR payload (see "Commissioning") |
| `p256` | | P-256 point arithmetic with Python ints for `commission`; not constant time, host only |
| `chainwalk` | | cost control for the uplink chain walk of the key store: time-bound gap, cache of walked keys, budgets (see "Forward secrecy in the key store") |
| `errors` | | the exceptions of the envelope (`SecureError` and its subclasses; `secure` re-exports them) |
| `diag` | `lib/sensei_diag` | reset cause and reason names, the 52 B fault record (`pack_fault`, `unpack_fault`), `addr2line` and the ELF lookup by image id. The job names of `last_job` belong to the app: pass `job_names` |
| `smp`, `smp_sim` | `NRF/system/ota` | MCUboot SMP client (serial and BLE links; upload, test, confirm, reset) and a simulated server. The board id of an image is a parameter: `load_image(source, kind=...)`; `python -m sensei_sdk.smp` is a small command line for it |
| `transport`, `transport_serial`, `transport_ble` | | byte transports (loopback, USB serial, BLE). The GATT UUIDs and the advertising name are a `BleProfile` that the app supplies |
| `vectors`, `vectors_commission` | | the C test vectors of the three libraries above (below), including the ratchet vectors of `sensei_secure` and the commissioning vectors V9 to V14 |

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
  USB provisioning. The reset gets a higher provisioning counter (`prov`, in the file): in a merge and in a follower the
  copy with the higher `prov` replaces the whole ratchet state, so another process that still holds generation 1 cannot
  write it back. `add_node(..., reset=False)` leaves a known node with the same key alone. `add_node` returns a
  `NodeBackup`; if the provisioning of the node then fails, `rollback_node(backup)` gives the old keys and ratchet state
  back (counters only go up). Provision in the file that the collector uses: a follower that calls `add_node` resets its
  own copy only.
- One responder per key store. A responder has an id (`responder_id=`, or the file in `SENSEI_REKEY_RESPONDER_ID_FILE`,
  made by `secure.load_responder_id(path)`: 16 random hex characters, keep it in the responder's own config folder, not in
  the key store folder). It is written to the key store file (`responder`) with every save. A responder that finds another
  id warns once a minute, counts it (`store.stats["responder_conflict"]`) and still answers; the last one to save holds the
  record. A copy of the store (GUI copy, restore from a backup) that also answers can send the node an answer that the
  other store does not know: the node uses the first one it gets, the other store then never promotes. Old files
  without the field stay readable.
- A node that asks again with a new key (it lost its state) gets a new answer that replaces the open one in `next`; the
  newer answer wins a merge with the file (`seq`). `NodeState.next_since` keeps the time of the first unanswered request
  while requests are replaced, so a node that never mixes shows its age. `NodeState.gen_since` is the time the generation became current (`cur.since` in the
  file; None for a file from before the field). The node twin (`NodeSession`) replaces a request
  after `ratchet.STALE_EPOCHS` (32768) epochs of sending, like the C library, and refuses the answer to the old one.
- Walking the chain for a far message costs one HMAC per epoch, and anyone can send a frame with a far counter. The
  limits (`chainwalk`): a message may be ahead by at most `min(SENSEI_E2E_MAX_SKIP, 4096 + 1 epoch per second since the
  last verified one)` epochs, else `GapError` at once; walked keys are cached at checkpoints every 1024 epochs (at most
  32 kB per chain, and a node can have three chains: current, previous, answered; LRU over all); a walk of up to 1024
  steps from a cached checkpoint (or the stored key) is free, longer walks are charged to budgets of 2^16 steps per node
  and 2^19 for the store per minute (`WalkBudgetError`, the walk goes on at the next frame), so forged frames next to a
  checkpoint cannot drain the budget of a real node; the budget is reserved before the walk, threads cannot spend it
  twice; the walk runs outside the store lock. Logs about refused frames are limited to one per node and minute;
  `store.stats` counts them.
- The time bound assumes at most 1 epoch per second (64 messages per second). A node that sends more over another link
  (BLE, USB) while the collector hears nothing for hours is refused with `GapError` at its next frame. For such a node
  `store.set_epoch_rate(node_id, rate)` raises the rate (not saved: set it at start).
- The chain key and `t_up` reach the file when the store saves (a promotion, an answer, a downlink, `flush`, or the autosave
  every `autosave_s`). After a crash the file holds the last saved state: forward secrecy lags by the epochs since then,
  and a frame of those epochs is accepted once more (replay window of the last save). A promotion that cannot be saved
  (disk error) keeps the verified message and is saved again with the next message or `flush`.
- A new sender id is checked against the senders in the file, so two hosts never share one.

## Commissioning

A node is added over BLE with a setup code printed on a card (11 digits and a Verhoeff check digit, `1234-5678-9012`).
The commissioner proves it knows the code with SPAKE2+ (RFC 9383, P-256), without sending it; the node stores only a
verifier made at the bench. Messages are plain bytes here: the app puts them
into its protobuf.

```python
from sensei_sdk import commission as com

# bench: code, verifier (scrypt, 32 MiB, about 0.1 s). v.encode() is the 121 B record that the node stores
code = com.generate_code()
v = com.make_verifier(code, node_id)
qr = com.qr_payload(node_id, code, com.node_fingerprint(node_id, node_pub))     # SN1:<id>:<code>:<fp>

# commissioner: nonce_g in commission_begin; the node answers with its params (salt, scrypt cost) and nonce_n
nonce_g = com.new_nonce()
p = com.Prover.from_code(code, node_id, params, nonce_g, nonce_n)    # WrongCodeError later if the code is wrong
share_p = p.share()
confirm_p = p.finish(share_v, confirm_v)    # InvalidPointError, WrongCodeError; StateError if out of order
ch = com.Channel(p.keys, com.PROVER)         # kind 5 envelopes: ch.seal(plain), ch.open(envelope)

# node side (tests, simulators)
s = v.session(node_id, nonce_g, nonce_n)
share_v, confirm_v = s.pake1(share_p)
s.pake3(confirm_p)
node_ch = com.Channel(s.keys, com.VERIFIER)

# after the key step
com.check_proof(proof, store, node_id, nonce_g, nonce_n)   # the node holds the key it sent; else ProofError
```

- The commissioner accepts only scrypt (log2n 14 to 17, r 8, p 1) and a 16 byte salt (`Params.check`).
- The channel is `secure.seal_raw` / `open_raw` with kind 5 (`KIND_COMMISSION`), key id = session id, no node id,
  generation 0, counters from 1, replay window 64, at most 320 bytes. The node sends with sender 0, the commissioner with
  sender 1. Every other receiver (`open_sealed`, `NodeSession.open`) refuses kind 5.
- Fingerprint of the node key: `node_fingerprint` (16 B, QR) and `fingerprint_text` (`a1b2 c3d4 e5f6 0718`).
  `collector_fingerprint` (8 B) tells whether a node is already ours.
- `generate_code` draws again for one repeated digit and for ascending or descending runs. `parse_code` and `parse_qr`
  check the check digit; the QR format is strict (upper case hex).
- `p256` and `commission` are not constant time. Use them on a PC at the bench or the office, never on a node.
- `open_sealed` reads the key store file again when it meets an unknown key id (at most once a second), so a node that
  another process added is found without a restart. This helps only when the nodes are added to the file that the reader
  uses. A GUI with its own key store file (or a `follow=` copy) writes new nodes to its own file: the collector does not
  see them.
- A QR payload never carries node id 0 (it means "any node" in `commission_begin`); `parse_qr` and `qr_payload` refuse it.

For the GUI (step E):

- Compare `node_fingerprint(node_id, node_pub)` with the fingerprint of the QR before `add_node`. `check_proof` proves the
  node holds the key it sent, not that the key belongs to this node id: the fingerprint carries that.
- On the typed-code path there is no QR to compare: show `fingerprint_text` and ask the user to confirm it explicitly.
- Decide "ours or foreign" only from the `collector_fp` inside the sealed identity (after the PAKE). `has_collector_key`
  and `attempts_left` in the plain params are not authenticated: show them as hints at most.
- The label texts and the QR format are bound to version 1 (`SN1`, `-v1` labels); a version 2 must change them.

## Tests and vectors

```sh
python -m pytest tools/python/tests        # or: make test-python
python -m sensei_sdk.vectors               # write the C test vectors
python -m sensei_sdk.vectors --check       # fail if they are out of date (CI)
```

The vectors go to `lib/sensei_link/tests/generated/vectors.h`, `lib/sensei_secure/tests/generated/vectors.h`,
`lib/sensei_secure/tests/generated/vectors_commission.h` and `lib/sensei_diag/tests/generated/vectors.h`; the host tests of the libraries check the C code against them
(`make test-host`). `tests/test_sdk_constants.py` compares the Python constants with the C headers. An app with
messages of its own builds its vectors on `vectors.pattern`, `array`, `pairs` and `frag_cases`.
