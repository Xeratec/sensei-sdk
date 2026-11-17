This example runs the test suite and a benchmark of the [SENSEI crypto library](../../NRF/system/crypto/README.md) on the nRF5340 and prints the results over RTT.

## NRF

Build and flash as described in the top-level [Readme](../../Readme.md#examples):
```sh
cd example/crypto_api/src_NRF
west build -b nrf5340_senseiv1_cpuapp
west flash
```

Expected output: one `[PASS]`/`[FAIL]` line per check, the line `ALL TESTS PASSED`, then the benchmark table (mean cycles and microseconds per operation for SHA-256, HMAC, HKDF, AES-256-GCM, ECDH and ECDSA).

Known-answer tests: SHA-256 (FIPS 180-2), HMAC-SHA256 (RFC 4231), HKDF-SHA256 (RFC 5869 cases 1 and 3), AES-GCM (NIST/McGrew-Viega cases 4, 13, 14, 16), ECDSA P-256 public key and signature verification (RFC 6979 A.2.5), ECDH P-256 public keys and shared secret (RFC 5903).

## Future work

The BLE secure-service demo of the original semester project (a TLS-like handshake over a GATT service with a Python client) is not ported. It depends on the TLS 1.2 PRF key schedule, which is not part of the library, and on a matching Python peer.

## Attribution

Tests and benchmark are ported from the semester project of Ahmed Malanovic (ETH Zurich, 2025).
