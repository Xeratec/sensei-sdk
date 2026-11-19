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

`src/sensei_ratchet.c` is the forward secrecy core (root key, mix, uplink chain; see "Forward secrecy"). On top of the envelope, `src/sensei_e2e.c` is the key store of a device (key pair, collector key, counters in flash), and
`zephyr/sensei_e2e_settings.c` keeps it in the Zephyr settings subsystem (see Key store below). `src/sensei_e2e_seal.c` is
the sealed record format for secret settings (keys at rest, see "Keys at rest").

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
1       1     kind      1 uplink, 2 downlink, 3 node to host data, 4 settings write (generation 1 and later: see below)
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
- Generation 0 is the envelope as described here. From generation 1 on (forward secrecy) byte 1 is
  `0x80 | (generation mod 4) << 4 | kind`; bit 6 must be 0 (an envelope with it set is refused). The size, the nonce and the
  associated data (the header bytes as sent, so the generation bits are authenticated) do not change. Read the kind with
  `sensei_secure_kind(buf)` (Python: `kind_of()`), never as `buf[1]`.
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
IEQ node, so a node that updates keeps its keys). Without `CONFIG_SENSEI_SECURE_SETTINGS_SEAL` the private key is plain in
flash: protect the chip (APPROTECT) in the field. ECC needs a few kB of stack: run the calls on a thread or
work queue with enough stack (the node uses one work queue, 6 kB).

### Keys at rest

`CONFIG_SENSEI_SECURE_SETTINGS_SEAL=y` stores the private key as the sealed record `e2e_privx` instead of `e2e_priv`
(`include/sensei_e2e_seal.h`, `src/sensei_e2e_seal.c`, mirror in Python: `sensei_sdk.secure.seal_record()`):

```
u8 version (1) | nonce[12] | AES-128-GCM ciphertext | tag[16]       29 B more than the plain value
associated data = "sensei-e2e/" || setting name || version
```

The name is in the associated data, so a record does not open in another place. The nonce is 12 random bytes of the backend
(new at every save). The key (16 B) comes from the function `int sensei_e2e_settings_seal_key(uint8_t key[16])`, which one of
these defines (a choice of Kconfig options, or the application):

- `CONFIG_SENSEI_SECURE_SETTINGS_SEAL_HUK` (`zephyr/sensei_e2e_huk.c`): nRF5340 application core. The first call writes a random
  hardware unique key into the KMU of the CryptoCell-312 (`hw_unique_key_write_random()`, once, until the chip is erased; the CPU
  cannot read it), then derives the key from the MEXT slot with the label `SENSEI-E2E-AT-REST-v1`. Selects `HW_UNIQUE_KEY`,
  `HW_UNIQUE_KEY_RANDOM` and `MPU_ALLOW_FLASH_WRITE`; needs `NRF_CC3XX_PLATFORM`. No TF-M. The crypto itself (ECDH, AES, SHA)
  runs on the Oberon driver; the CryptoCell provides the random numbers and holds the hardware key.
- `CONFIG_SENSEI_SECURE_SETTINGS_SEAL_TEST_KEY` (`zephyr/sensei_e2e_testkey.c`): a fixed key that a test can change
  (`sensei_e2e_testkey_set()`) or make fail, for `native_sim`. Never for a product.

The settings code imports the key once as a volatile PSA key and wipes the copy in RAM. Behaviour:

- `sensei_e2e_settings_load()` opens the record. A plain `e2e_priv` of an earlier version is migrated at the first start: sealed
  to `e2e_privx`, read back and compared. The key and the key id do not change. The plain record is not deleted yet: the new
  image may be only on trial, and if MCUboot goes back to the old image, that image needs its key. The application calls
  `sensei_e2e_settings_confirm()` once the image is confirmed (after `boot_write_img_confirmed()`; calling it at every start is
  fine), and that deletes the plain record (`sensei_e2e_settings_plain_pending()` says if one is waiting). Two records with the
  same key are the normal state before the confirmation. If they hold different keys, an older firmware made a new plain key
  after the key was sealed (downgrade, then upgrade; that firmware does not know
  `e2e_privx`, and the collector was provisioned for its key): the plain key is the newer one and is sealed over the old one
  (log line "the plain key is newer"). A plain record that is not a valid P-256 scalar (`sensei_secure_priv_valid()`: 0 or at
  or above the group order) is damaged: it is never sealed. Without a sealed record the key store replaces it with a new key
  pair (sealed, the plain record deleted); next to a sealed key it is junk and the sealed key stands. The old plain value stays in the flash page until the garbage collection of the
  settings: a device whose flash others could read must be reset.
- If the key is not available, or the record does not open (changed, sealed for another name, wrong size, another chip),
  `nv->key_unreadable` is set, `nv->have_priv` is false, and `sensei_e2e_init()` returns `-EACCES` without making a new key pair
  and without touching the stored record. The device then has no end-to-end security (visible to the caller). Only
  `sensei_e2e_reset()` starts over. A provider that fails when there is no key yet makes `sensei_e2e_init()` fail with its error
  and stores nothing.
- A key that is stored sealed (`nv->priv_sealed`: it opened from `e2e_privx`, or it was a valid plain key that was just sealed)
  and cannot be turned into a public key (a crypto error such as no free PSA key slot) is not "damaged": `sensei_e2e_init()`
  returns `-EIO`, keeps the record and does not make a new key pair. A plain key that is not a valid scalar is damaged, is not
  sealed and has `priv_sealed` false, so it is replaced.
- `sensei_e2e_reset()` erases the stored key before it makes a new one. Call `sensei_e2e_settings_reset_check()` first: it asks
  the provider again (also after a failure that was cached at the start) and returns 0 only if a new key can be sealed. Without
  it a reset with no sealing key would end with no key at all. The erase stops at the first record that cannot be deleted
  (`struct sensei_e2e_ops.erase` returns an error; order: collector key, key records, counters last, so a counter never comes
  back with a key that survived). Then `sensei_e2e_reset()` is refused: it returns that error and the session and the state in
  RAM stay as they were; some records may be gone, so call it again. A new key pair (first start, damaged key) deletes the plain
  `e2e_priv` before it writes the sealed record, and fails if the delete fails.
- `sensei_e2e_settings_seal_prepare()`: call it early in `main()`, before other threads write flash. The HUK provider writes
  the hardware unique key there if there is none (`hw_unique_key_write_random()` writes the UICR through the NVMC inside the
  CryptoCell library, without the flash driver lock). Without the call the first key request does it. A HUK that was only
  partly written (power cut during the very first start) gives `HW_UNIQUE_KEY_ERR_MISSING` for good: only a chip erase
  (`nrfutil device recover`) helps.
- The name `e2e_rat` is reserved for the ratchet state of a later version (sealed the same way). `erase` deletes it.
- `sensei_e2e_settings_seal()`, `_unseal()`, `_seal_status()` and `_seal_close()` are the pieces, for the application and tests.
  Call everything from the one thread that uses the key store.

What it adds: a flash image copied to another chip, and stale settings pages, do not give up the key. What it does not stop: code
that runs on the chip (open debug port, glitch, a malicious signed image) can use the provider and read the key while it is in use.
APPROTECT stays the main barrier. A record for a later version of the format has another version byte and is refused.

## Forward secrecy (`include/sensei_ratchet.h`, `src/sensei_ratchet.c`)

The keys of generation 0 (above) stay as they are. Generation 1 and later come from a ratchet, so a stolen key does not open
older messages. This is the core: the primitives, the envelope bits and the hook for the keys. The device key store that keeps
the state in flash, and the collector side, are built on it. All integers are u32 little endian, labels ASCII.

```
R_0     = HKDF(ikm = k_up || k_down, salt = "SENSEI-E2E-root-v1", info = key_id, L = 32)
mix to generation g+1, with an ephemeral X25519 key of the node (e_N) and of the collector (e_C):
  dh    = X25519(e_N_priv, e_C_pub) = X25519(e_C_priv, e_N_pub)            all zero: refused
  th    = "SENSEI-RK-v1" || key_id || gen(g+1) || up_base || e_N_pub || e_C_pub          88 B
  okm   = HKDF(ikm = dh, salt = R_g, info = th, L = 80)
  R_{g+1} = okm[0:32]   CKu_{g+1,0} = okm[32:64]   k_down_{g+1} = okm[64:80]
uplink chain: epoch(c) = c >> 6, i = epoch(c) - up_base (below 0: refused)
  MK_i = HMAC(CK_i, 0x01)[0:16]   (AES-128-GCM key of the 64 counters of epoch i)      CK_{i+1} = HMAC(CK_i, 0x02)
```

- An epoch is 64 counters, the size of the replay window. The nonce and the counters do not change (counters stay global per
  node and never repeat under any key); an uplink in epoch i uses MK_i. A downlink uses `k_down` of its generation, no chain.
- `sensei_ratchet_root0`, `_transcript`, `_mix_dh`, `_mix` (both roles), `_public`, `_dh` (all-zero result gives
  `SENSEI_SECURE_ERR_DH`; a backend may also refuse a low-order point itself), `_mk`, `_next`, `struct sensei_ratchet_chain`
  with `_chain_init`, `_chain_advance`, `_chain_key` (without `commit` the walk is done on a copy, for use before the tag is
  checked), `_chain_wipe`. A walk of more than `SENSEI_RATCHET_MAX_SKIP` (2^20) epochs gives `SENSEI_SECURE_ERR_GAP`; an epoch
  before the chain start or behind the chain gives `_ERR_EPOCH`. Python: `sensei_sdk.ratchet` (same names, `Chain`).
- The cipher backend has three more functions: `hmac_sha256`, `x25519_keygen`, `x25519` (OpenSSL; PSA with
  `PSA_WANT_ECC_MONTGOMERY_255`, `CONFIG_SENSEI_CRYPTO_X25519` in the crypto library; Python `cryptography`). `x25519` clamps
  the scalar and ignores the top bit of u (RFC 7748). A backend without them leaves them NULL (`sensei_ratchet_available()`).
  Zephyr: `CONFIG_SENSEI_SECURE_RATCHET` (default y) selects them and builds `sensei_ratchet.c`.
- Envelope: generation 0 is unchanged byte for byte. From generation 1 on the kind byte has the ratchet bit and the generation
  mod 4 (see Envelope). `sensei_secure_kind()`, `_is_ratchet()`, `_gen_bits()` read it; `sensei_secure_peek()` reads the whole
  header without opening it (kind, generation bits, key id, sender, counter).
- Keys of the generations: `struct sensei_secure_keyring` (`sensei_secure_set_keyring()` after init). `tx_key(counter)` is
  called by seal after it took the counter and returns the generation and the 16 B key (the message key of the epoch, or the
  downlink key); `rx_key(ratchet, gen_bits, counter)` is called by open after the checks of the header and the replay window
  and before the tag, and returns the key, or "use the session key" (generation 0 only), or an error. Without a keyring a
  ratchet envelope is refused with `SENSEI_SECURE_ERR_GEN`. The hook is called before the tag is checked, so it must not
  change its state for an unauthenticated envelope: use `_chain_key` without `commit`, and commit after `sensei_secure_open()`
  returned 0.
- `sensei_secure_tx_jump(s, next)` raises the sending counter (never lowers it): an epoch jump after a reboot or an old epoch,
  or the first epoch of a new generation (`next = 64 * up_base`).
- Not in this version: the device state machine (request, answer, switch, flash records), the Python key store v3, the node and
  the collector. The test `tests/test_ratchet.c` has a small model of both sides for the scripted transcript.

## API (`include/sensei_secure.h`)

`sensei_secure_keygen`, `_derive`, `_priv_valid`, `_init` (clears the struct, takes the sender, counters, table of senders and the
storage callbacks), `_close`, `_set_keyring`, `_tx_jump`, `_seal`, `_open`, `_peek`, `_peek_key_id`, `_kind`, `_rx_highest`,
`_replay_check`, `_replay_update`. The key
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
  `tests/generated/vectors.h`. The vectors come from the Python implementation in `tools/python`
  (`python -m sensei_sdk.vectors`, `make vectors`; CI checks that they are current); do not edit the file by hand.
- `test_e2e` checks the key store (`sensei_e2e`) with a fake flash: first start, provisioning, key A then B then A,
  two senders, failing writes, full table of senders, clearing and the floor, reset, a damaged key, a sealed key with a crypto error, an unreadable key and a key
  that cannot be stored.
- `test_ratchet` checks the ratchet against the Python vectors (vectors 1 to 6 of the forward secrecy design): R_0, X25519 of
  RFC 7748 (sections 5.2 and 6.1) and low-order public keys, the mix on both sides, the chain (CK_0..CK_3, MK_0..MK_3, CK_1000),
  envelopes of generation 1 and 4 (the generation bits wrap) with refusals (reserved bit, generation bits without the ratchet
  bit, epoch before the chain, wrong generation, tag), the keyring hook, `tx_jump`, a scripted transcript (generation 0, request,
  answer, late generation 0 uplink, generation 1 across epoch jumps, previous generation downlink, previous generation dropped),
  and a property test: no (key, nonce) pair twice over 100 000 uplinks, 20 mixes and 40 000 downlinks of 8 senders, and no
  message key for more than 64 counters.
- `test_seal` checks the sealed settings records against the Python vectors (vector 7, `tests/generated/vectors.h`): the same bytes
  both ways, every byte covered by the tag, wrong name and key, short and long records, new nonce at every seal.

`test_ratchet.c` and `test_secure.c` also check the generation 0 bytes and the refusals of the kind byte. The same test files run with the Zephyr backend (PSA, Mbed TLS) on `native_sim`: `NRF/tests/secure` (`test_secure`) and
`NRF/tests/secure_settings` (the settings storage: names, sizes, restart, erase; a second build with sealing and the fixed-key
provider: sealed names, a record that is changed or does not belong, another key, a failing provider, the migration of the plain key
and its deletion at the confirmation, a damaged plain key)
(`make twister`).
