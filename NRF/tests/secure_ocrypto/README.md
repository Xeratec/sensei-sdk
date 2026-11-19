<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# secure_ocrypto

Build check of the commissioning (`lib/sensei_secure`, SPAKE2+) with the `ocrypto` backend on the nRF5340 application
core. It has no `testcase.yaml`: Twister cannot build the SENSEI board without help, and nothing runs on hardware here.
`main()` is a self-check for the first run on a board (untested on hardware): the ocrypto point arithmetic against the
RFC 9383 vectors, and a node session against a recorded one; it prints `ocrypto self-check: PASS` or `FAIL` lines.

```sh
export SENSEI_SDK_ROOT=<this repository>
west build -b nrf5340_senseiv1/nrf5340/cpuapp --no-sysbuild -d build/full NRF/tests/secure_ocrypto \
    -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF
# what a node pays: only the calls of the node, no vectors, no commissioner side
west build ... -d build/node ... -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF -DEXTRA_CFLAGS=-DNODE_FOOTPRINT
# the base to compare with: the key store of the node without the commissioning
west build ... -d build/base ... -- -DEXTRA_ZEPHYR_MODULES=$SENSEI_SDK_ROOT/NRF -DCONFIG_SENSEI_SECURE_COMMISSION=n
```

Flash (NCS v3.4.1, `zephyr.bin`): base 89544 B, node 97492 B (+7948 B), full with self-check and vectors 102788 B.
Static RAM does not change (the sessions live where the caller puts them).
