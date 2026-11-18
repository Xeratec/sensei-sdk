<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Docker images

| Image | Dockerfile | Content |
|---|---|---|
| `sensei-nrf:ncs-3.4.1` | `nrf.Dockerfile` | Ubuntu 24.04 (by digest), Zephyr SDK 1.0.1 (ARM toolchain), west 1.5.0, CMake 4.4.5 and the Python requirements of nRF Connect SDK v3.4.1 (versions from `../requirements-constraints.txt`). No sources: mount a west workspace at `/workspace`. |
| `sensei-twister:ncs-3.4.1` | `twister.Dockerfile` | the image above plus a host compiler (native_sim) and QEMU (Cortex-M tests) |

```sh
scripts/docker/build.sh nrf [--if-missing]
scripts/docker/build.sh twister [--if-missing]
```

`SENSEI_NRF_IMAGE` and `TWISTER_IMAGE` change the names. `scripts/twister.sh` builds the twister image (and, under it,
the nrf image) when it is missing.

Apps build on these images. An app that builds its firmware in Docker uses `sensei-nrf` as it is. The node repository's
simulator image (`sensei-nodesim`) adds `socat` to the twister image.

Run a build in the nrf image with a west workspace of your own:

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "<workspace>:/workspace" -w /workspace/<app> \
  sensei-nrf:ncs-3.4.1 west build ...
```

The apt packages are those of the build day. To move to a newer Ubuntu base, take the digest of `ubuntu:24.04`
(`docker buildx imagetools inspect ubuntu:24.04`), rebuild and test.
