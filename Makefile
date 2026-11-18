# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  format    - Format all files with the pre-commit hooks (clang-format, yapf, isort, ...)"
	@echo "  test-host - Build and run the host tests of lib/ and NRF/system/ (CMake, ctest)"
	@echo "  test-python - Run the Python tests (tools/python, GAP9/ssbl/tools; pytest) and check the C test vectors"
	@echo "  vectors   - Write the C test vectors of lib/ from the Python package (tools/python)"
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

# Python tests: the package sensei_sdk (tools/python; needs pytest and cryptography), then the GAP9 image builder
# (GAP9/ssbl/tools). The builder tests that sign an image or run the writer also need cryptography, imgtool ($IMGTOOL)
# and a C compiler, and skip without them (with $CI set they fail instead). The last step fails if a committed C test
# vector (lib/*/tests/generated) is out of date. .github/workflows/python-tests.yml runs this target.
test-python:
	PYTHONPATH=tools/python python3 -m pytest tools/python/tests -q
	python3 -m pytest GAP9/ssbl/tools -q
	PYTHONPATH=tools/python python3 -m sensei_sdk.vectors --check

# C test vectors of lib/sensei_link, lib/sensei_secure and lib/sensei_diag, written by the Python implementations.
vectors:
	PYTHONPATH=tools/python python3 -m sensei_sdk.vectors

# Zephyr tests (NRF/tests) with Twister on native_sim and QEMU (qemu_cortex_m3), in Docker. Arguments for Twister: make twister ARGS="-s sensei.busarb"
twister:
	@scripts/twister.sh -- $(ARGS)

twister-coverage:
	@scripts/twister.sh --coverage -- $(ARGS)

.PHONY: help format test-host test-python vectors twister twister-coverage
