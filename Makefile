# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  format    - Format all files with the pre-commit hooks (clang-format, yapf, isort, ...)"
	@echo "  test-host - Build and run the host tests of lib/ and NRF/system/ (CMake, ctest)"
	@echo "  twister   - Run the Zephyr tests of NRF/tests on native_sim and QEMU in Docker (scripts/twister.sh)"
	@echo "  twister-coverage - Same with code coverage of the SDK sources (native_sim): build/twister/twister-out/coverage"
	@echo "  help      - Display this help message"

# Needs pre-commit. It fixes files in place; the exit code is 1 when it changed something.
format:
	pre-commit run --all-files

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

twister-coverage:
	@scripts/twister.sh --coverage -- $(ARGS)

.PHONY: help format test-host twister twister-coverage