<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# secure_ocrypto

Build check of the commissioning (`lib/sensei_secure`, SPAKE2+) with the `ocrypto` backend on the nRF5340 application
core (SENSEI board). Build only: nothing runs on hardware here. `main()` is a self-check for the first run on a board
(untested on hardware): the ocrypto point arithmetic against the RFC 9383 vectors, and a node session against a recorded
one; it prints `ocrypto self-check: PASS` or `FAIL` lines.

Three Twister scenarios (`testcase.yaml`; the default platforms of `scripts/twister.sh` do not include the board):

```sh
TWISTER_PLATFORM=nrf5340_senseiv1/nrf5340/cpuapp scripts/twister.sh -- \
    -s sensei.secure.ocrypto -s sensei.secure.ocrypto.node -s sensei.secure.ocrypto.base
```

- `sensei.secure.ocrypto`: with the self-check, the vectors and the commissioner side.
- `.node`: only the calls of the node (`-DNODE_FOOTPRINT`), no vectors. Its size minus `.base` is what a node pays.
- `.base`: the key store of the node without the commissioning (`CONFIG_SENSEI_SECURE_COMMISSION=n`).

Without Twister: `west build -b nrf5340_senseiv1/nrf5340/cpuapp --no-sysbuild NRF/tests/secure_ocrypto -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF`
(add `-DEXTRA_CFLAGS=-DNODE_FOOTPRINT` or `-DCONFIG_SENSEI_SECURE_COMMISSION=n` for the other two).

Flash (NCS v3.4.1, `zephyr.bin`): base 89544 B, node 96660 B (+7116 B), full 101988 B. Static RAM does not
change (the sessions live where the caller puts them). SHA-256 goes through the incremental PSA calls
(`sensei_crypto_sha256()`), so it shares the code that HMAC and HKDF already have in the image.
