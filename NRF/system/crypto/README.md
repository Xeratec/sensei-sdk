<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# SENSEI Crypto Library

A small, consistent C API for common cryptographic operations on the nRF5340. It is a thin layer over the PSA Crypto API of nRF Security (nRF Connect SDK v2.6.1) and derives from the semester project of Ahmed Malanovic (see [Attribution](#attribution)).

## Features

| Group | Functions | Kconfig |
|-------|-----------|---------|
| Core | `sensei_crypto_init`, `_random`, `_destroy_key`, `_sha256` | `CONFIG_SENSEI_CRYPTO` |
| AES-GCM (128/256 bit) | `_aead_import_key`, `_aead_generate_key`, `_aead_generate_nonce`, `_aead_encrypt`, `_aead_decrypt` | `CONFIG_SENSEI_CRYPTO_AEAD` |
| HMAC-SHA256 | `_hmac_sha256` | `CONFIG_SENSEI_CRYPTO_HMAC` |
| HKDF-SHA256 (RFC 5869) | `_hkdf_sha256` | `CONFIG_SENSEI_CRYPTO_HKDF` (selects HMAC) |
| ECDH P-256 | `_ecdh_generate_keypair`, `_ecdh_import_private_key`, `_ecdh_export_public_key`, `_ecdh_shared_secret` | `CONFIG_SENSEI_CRYPTO_ECDH` |
| X25519 (RFC 7748) | `_x25519`, `_x25519_keygen` | `CONFIG_SENSEI_CRYPTO_X25519` (off by default; `CONFIG_SENSEI_SECURE_RATCHET` selects it) |
| ECDSA P-256 / SHA-256 | `_ecdsa_generate_keypair`, `_ecdsa_import_private_key`, `_ecdsa_import_public_key`, `_ecdsa_export_public_key`, `_ecdsa_sign_hash`, `_ecdsa_verify_hash`, `_ecdsa_sign`, `_ecdsa_verify` | `CONFIG_SENSEI_CRYPTO_ECDSA` |

All functions return one `sensei_crypto_status_t`. Keys live inside PSA, the library keeps no key material. Functions that create keys return a key id that the caller releases with `sensei_crypto_destroy_key()`. Private keys cannot be exported. See [`sensei_crypto.h`](sensei_crypto.h) for the documented API.

## Usage

Enable the library in `prj.conf`. The sub-options default to `y` and can be switched off to save flash. The options select `PSA_CRYPTO` (nRF Security with Oberon PSA Crypto on the nRF53, Mbed TLS 4 / TF-PSA-Crypto API) and the required `PSA_WANT_*` symbols.

```
CONFIG_SENSEI=y
CONFIG_SENSEI_CRYPTO=y
CONFIG_MBEDTLS_ENABLE_HEAP=y
CONFIG_MBEDTLS_HEAP_SIZE=16384
CONFIG_MAIN_STACK_SIZE=16384
```

The application sizes the mbed TLS heap and the stack of the calling thread (ECC operations need several kB). Set `CONFIG_SENSEI_CRYPTO_LOG_LEVEL_DBG=y` to log the underlying PSA error codes.

```c
#include "crypto/sensei_crypto.h"

sensei_crypto_init();

uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE];
uint8_t ct[sizeof(msg) + SENSEI_CRYPTO_AES_GCM_TAG_SIZE];
size_t ct_len;
sensei_crypto_key_id_t key;

sensei_crypto_aead_import_key(key_bytes, 32, &key);
sensei_crypto_aead_generate_nonce(nonce);
sensei_crypto_aead_encrypt(key, nonce, NULL, 0, msg, sizeof(msg), ct, sizeof(ct), &ct_len);
sensei_crypto_destroy_key(key);
```

Notes:
- The AES-GCM output is the ciphertext followed by the 16 byte tag. Never reuse a nonce with the same key.
- The raw ECDH secret is not uniformly random. Pass it through `sensei_crypto_hkdf_sha256()`.
- ECDSA uses randomized signatures (not RFC 6979 deterministic mode).

## Example

[`example/crypto_api`](../../../example/crypto_api) runs a test suite (known-answer tests from NIST, RFC 4231, RFC 5869, RFC 6979 and RFC 5903, plus round-trip and negative tests) and a short benchmark of every operation. The same known-answer tests run on `native_sim` (PSA with Mbed TLS) in `NRF/tests/crypto` (`make twister`).

## Not included

The TLS 1.2 PRF helpers, the handshake constants, SHA-384 and the BLE secure-service demo of the semester project are not part of the library.

## Attribution

Derived from the semester project of Ahmed Malanovic (ETH Zurich, 2025). Licensed under Apache-2.0, copyright ETH Zurich and University of Bologna.
