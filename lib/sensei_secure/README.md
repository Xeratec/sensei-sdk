<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# sensei_secure

End-to-end envelope for sensor data: P-256 ECDH, HKDF-SHA256, AES-128-GCM and a replay window. C99, no
allocation, no Zephyr in the core (`src/sensei_secure.c`). The cipher comes in through
`struct sensei_secure_crypto`, with two backends:

- `src/sensei_secure_zephyr.c`: the SDK crypto library (`NRF/system/crypto`, PSA Crypto), for nRF.
- `src/sensei_secure_openssl.c`: OpenSSL 3, for host tests and tools.

On top of the envelope, `src/sensei_e2e.c` is the key store of a device (key pair, collector key, counters in flash), and
`zephyr/sensei_e2e_settings.c` keeps it in the Zephyr settings subsystem (see Key store below).

## Keys

The device makes a P-256 key pair (`sensei_secure_keygen()`); the collector has one too. Both sides derive the
same keys:

```
secret = ECDH(own private, peer public)                       32 B
okm    = HKDF-SHA256(ikm = secret, salt = "SENSEI-E2E-v1", info = node_pub || backend_pub, L = 36)
k_up = okm[0:16]   k_down = okm[16:32]   key_id = u32 LE okm[32:36]   (0 is refused)
```

`node_pub` and `backend_pub` are 65 B uncompressed points (`04 || X || Y`). `k_up` (AES-128-GCM) seals node to
backend, `k_down` backend to node. Every key is used in one direction only.

## Envelope

One layout on every link. All integers are little endian.

```
offset  size  field
0       1     flags     bits 7..4 = 0xA (marker), bits 3..2 = version (3),
                        bit 1 = node id present, bit 0 = direction (0 node to backend, 1 backend to node)
1       1     kind      1 uplink, 2 downlink, 3 node to host data, 4 settings write
2       8     node_id   only if flags bit 1 is set
n       4     key_id
n+4     4     sender    only backend to node (direction 1), never 0
n+4/8   4     counter   per sender, starts at 1
...     L     ciphertext (same length as the plaintext)
...     16    GCM tag
```

`n` is 2, or 10 with the node id. Overhead: 26 B node to backend (34 B with the node id), 30 B backend to node.

- Nonce (12 B): `direction (1) || 0x00 x 3 || sender (4) || counter (4)`. A node to backend envelope has sender 0. Two
  sealing hosts have different senders, so they never share a nonce.
- Associated data: all header bytes as sent.
- The kinds say what an envelope is for; what each link seals is up to the app. `sensei_secure_open()` takes the kind
  that is allowed there.
- `sensei_secure_is_sealed()` checks the marker and the version bits. Versions are odd (1, 3), so an envelope starts
  with 0xAC ... 0xAF. An encoded protobuf message starts with a tag below 0x80, and a plain Settings patch can start with
  0xA0 or 0xA8 (varint fields 20, 21, 36, 37, 52, 53) but never with 0xAC to 0xAF.
- Why a fixed binary header and not a protobuf message: about 10 B against 17 B, and the associated data is exactly the
  bytes on the wire, with no re-encoding to agree on.
- Why a 32-bit counter: at one message per second it lasts 136 years, and it costs 4 B less on every LoRa message. The
  sender refuses to send at 2^32 - 1 (new keys and a reset are needed).
- Why a 32-bit sender in downlinks only: uplinks have one sender, the node. A random 32-bit id makes a clash between two
  hosts negligible (a clash would reuse a nonce), and it costs 4 B only on the rare downlink.

## Counters and replay

- The sender persists a high-water mark every `ctr_step` messages through `store.tx`. After a reboot it sends above the
  mark, so up to `ctr_step` counters are skipped (a gap is harmless). If the write fails, the message is not sent. The mark
  belongs to the sender, not to the key: never lower it when the key changes.
- The receiver keeps, per sender (up to `SENSEI_SECURE_MAX_SENDERS`, 8), the highest counter and a 64-bit bitmap of the 64
  counters up to it. A counter is accepted once if it is above the highest, or inside the window and not seen. Counter 0
  and anything 64 behind is dropped. The tag is checked first; the window changes only after a valid tag.
- The receiver stores each new highest counter through `store.rx`; if that fails the message is refused. After a reboot a
  window counts as fully seen, so nothing at or below the stored highest counter is accepted again.
- A ninth sender is refused (only a message with a valid tag can take a slot).

## Key store (`sensei_e2e.h`)

`sensei_e2e` is the device side on top of the envelope: it makes the key pair on first start, keeps the collector key,
opens the session and persists the counters. No Zephyr and no allocation; the cipher and the storage come in through
`struct sensei_secure_crypto` and `struct sensei_e2e_ops`.

- `sensei_e2e_init()` takes what was read from flash (`struct sensei_e2e_nv`, may be empty), makes the key pair if there is
  none (a damaged key is replaced) and opens the session if a collector key is stored.
- `sensei_e2e_provision()` stores the collector key and derives the session. A bad key gives `-EINVAL` and changes
  nothing; provisioning again, with the same or another key, goes on above the sent mark and keeps all windows.
- `sensei_e2e_seal()` and `sensei_e2e_open()` return the length or a negative errno (`-ENOTCONN` no session, `-EACCES` tag
  or key id, `-EALREADY` replay, `-EBADMSG` format, `-ENOSPC`, `-ENOMEM` sender table full, `-EIO`).
- `sensei_e2e_clear_senders()` empties the table of senders (a full table refuses every new sender). The highest counter of
  the cleared entries becomes a floor, kept in flash: from then on a downlink at or below it is refused (`-EALREADY`)
  whatever its sender, so recorded downlinks cannot be replayed. The floor only rises. A host that takes a new sender must
  go on from the counter of the device.
- `sensei_e2e_reset()` erases the stored state and makes a new key pair.
- The callbacks of `struct sensei_e2e_ops` are called one at a time. The RAM state follows the flash state: if a write
  fails, the call fails and the state stays as it was.

Zephyr: `CONFIG_SENSEI_SECURE_SETTINGS=y` adds the storage on the settings subsystem (`sensei_e2e_settings.h`):
`sensei_e2e_settings_ops` and `sensei_e2e_settings_load()`. The keys are `<prefix>/e2e_priv`, `e2e_bpub`, `e2e_tx`,
`e2e_rxs` and `e2e_rxf`; the prefix is `CONFIG_SENSEI_SECURE_SETTINGS_PREFIX` (default `sensei_comm`, the one of the SENSEI
IEQ node, so a node that updates keeps its keys). The private key is not encrypted at rest: the SDK crypto library has no
persistent keys, so protect the chip (APPROTECT) in the field. ECC needs a few kB of stack: run the calls on a thread or
work queue with enough stack (the node uses one work queue, 6 kB).

## API (`include/sensei_secure.h`)

`sensei_secure_keygen`, `_derive`, `_init` (clears the struct, takes the sender, counters, table of senders and the
storage callbacks), `_close`, `_seal`, `_open`, `_peek_key_id`, `_rx_highest`, `_replay_check`, `_replay_update`. The key
store has its own header, `sensei_e2e.h`. Backends:
`sensei_secure_crypto_zephyr()` (`SENSEI_SECURE_BACKEND_ZEPHYR`) and `sensei_secure_crypto_openssl()`
(`SENSEI_SECURE_BACKEND_OPENSSL`).

## Use

**Zephyr**: `CONFIG_SENSEI=y`, `CONFIG_SENSEI_SECURE=y` (selects `SENSEI_CRYPTO` with AEAD, ECDH and HKDF).
Size the mbed TLS heap and the stack of the calling thread as described in `NRF/system/crypto/README.md`. The
application includes `sensei_secure.h` (add `lib/sensei_secure/include` to its include path if it also builds
without the library).

**Host**: `add_subdirectory(<sdk>/lib/sensei_secure sensei_secure)` and link `sensei_secure`. Needs OpenSSL 3
(on macOS `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`); without it `SENSEI_SECURE_FOUND` is OFF.

## Tests

`make test-host` at the SDK root, or `cmake -S lib/sensei_secure/tests -B build && cmake --build build && ctest
--test-dir build`.

- `test_secure` checks keys, envelopes (both directions), tampering, replay and counters against the vectors in
  `tests/generated/vectors.h`. The vectors come from the Python implementation in the SENSEI IEQ node repository
  (`python -m sensei_host.vectors`); do not edit the file by hand.
- `test_e2e` checks the key store (`sensei_e2e`) with a fake flash: first start, provisioning, key A then B then A,
  two senders, failing writes, full table of senders, clearing and the floor, reset, a damaged key.

The same test files run with the Zephyr backend (PSA, Mbed TLS) on `native_sim`: `NRF/tests/secure` (`test_secure`) and
`NRF/tests/secure_settings` (the settings storage: names, sizes, restart, erase) (`make twister`).
