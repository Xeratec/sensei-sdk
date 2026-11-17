# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  format    - Format code using clang-format and yapf"
	@echo "  test-host - Build and run the host tests of lib/ and NRF/system/ (CMake, ctest)"
	@echo "  twister   - Run the Zephyr tests of NRF/tests on native_sim and QEMU in Docker (scripts/twister.sh)"
	@echo "  help      - Display this help message"

format:
	@echo "Formatting code..."
	@python3 ./scripts/run_clang_format.py -ri .
	@python3 -m yapf -rpi .

# Every lib/<name>/tests/CMakeLists.txt and NRF/system/<name>/tests/CMakeLists.txt is a host test suite. Needs CMake, Ninja and, for sensei_secure, OpenSSL 3.
BUILD_DIR ?= build

test-host:
	@set -e; for t in lib/*/tests/CMakeLists.txt NRF/system/*/tests/CMakeLists.txt; do \
	  src=$$(dirname $$t); name=$$(basename $$(dirname $$src)); \
	  echo "== $$name"; \
	  cmake -S $$src -B $(BUILD_DIR)/host/$$name -G Ninja >/dev/null; \
	  cmake --build $(BUILD_DIR)/host/$$name; \
	  ctest --test-dir $(BUILD_DIR)/host/$$name --output-on-failure; \
	done

# Zephyr tests (NRF/tests) with Twister on native_sim and QEMU (qemu_cortex_m3), in Docker. Arguments for Twister: make twister ARGS="-s sensei.busarb"
twister:
	@scripts/twister.sh -- $(ARGS)

.PHONY: help format test-host twister