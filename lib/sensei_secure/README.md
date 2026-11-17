# sensei_secure

End-to-end envelope for sensor data: P-256 ECDH, HKDF-SHA256, AES-128-GCM and a replay window. C99, no
allocation, no Zephyr in the core (`src/sensei_secure.c`). The cipher comes in through
`struct sensei_secure_crypto`, with two backends:

- `src/sensei_secure_zephyr.c`: the SDK crypto library (`NRF/system/crypto`, PSA Crypto), for nRF.
- `src/sensei_secure_openssl.c`: OpenSSL 3, for host tests and tools.

## Keys

The node makes a P-256 key pair (`sensei_secure_keygen()`); the collector has one too. Both sides derive the
same keys:

```
secret = ECDH(own private, peer public)
okm    = HKDF-SHA256(ikm = secret, salt = "SENSEI-E2E-v1", info = node_pub || backend_pub, L = 36)
k_up = okm[0:16]   k_down = okm[16:32]   key_id = u32 LE okm[32:36]   (0 is refused)
```

Public keys are 65 B uncompressed points. `k_up` seals node to backend, `k_down` backend to node.

## Envelope

Little endian. `flags | kind | [node_id (8)] | key_id (4) | [sender (4)] | counter (4) | ciphertext | tag (16)`

- `flags`: bits 7..4 = 0xA (marker), bits 3..2 = version (3), bit 1 = node id present, bit 0 = direction.
- `kind`: uplink, downlink, node to host, settings. Overhead 26 B (34 B with the node id), 30 B backend to node.
- `sender`: only backend to node, never 0. Every sealing host has its own id, so two hosts never share a nonce.
- Nonce (12 B): direction, three zero bytes, sender (0 for the node), counter. The header is the associated data.
- Replay: the receiver keeps, per sender (up to `SENSEI_SECURE_MAX_SENDERS`, 8), the highest counter and a 64-bit
  bitmap. A ninth sender is refused. The tag is checked before the window changes.
- Counters: the sender persists a high-water mark every `ctr_step` messages through `store.tx`. The mark belongs to
  the sender, not to the key: never lower it when the key changes. The receiver stores each new highest counter
  through `store.rx`; if that fails the message is refused.

`sensei_secure_is_sealed()` checks the marker and the version bits. Versions are odd (1, 3), so an envelope starts with
0xAC ... 0xAF; a plain Settings patch can start with 0xA0 or 0xA8 but never with those.

## API (`include/sensei_secure.h`)

`sensei_secure_keygen`, `_derive`, `_init` (clears the struct, takes the sender, counters, table of senders and the
storage callbacks), `_close`, `_seal`, `_open`, `_peek_key_id`, `_rx_highest`, `_replay_check`, `_replay_update`. Backends:
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
--test-dir build`. `test_secure` checks keys, envelopes (both directions), tampering, replay and counters
against the vectors in `tests/generated/vectors.h`. The vectors come from the Python implementation in the SENSEI
IEQ node repository (`python -m sensei_host.vectors`); do not edit the file by hand.
