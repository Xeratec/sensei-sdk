# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""The C test vectors in the SDK come from sensei_sdk.vectors and are committed."""

from sensei_sdk import vectors


def test_vectors_are_current():
    assert vectors.main(["--check"]) == 0


def test_check_finds_a_stale_header(tmp_path, capsys):
    assert vectors.main(["--check", "--sdk-root", str(tmp_path)]) == 1  # nothing written there
    assert vectors.main(["--sdk-root", str(tmp_path)]) == 0
    assert vectors.main(["--check", "--sdk-root", str(tmp_path)]) == 0
    stale = tmp_path / vectors.LINK_HEADER
    stale.write_text(stale.read_text() + "\n")
    assert vectors.main(["--check", "--sdk-root", str(tmp_path)]) == 1
    assert "out of date" in capsys.readouterr().err
    # written twice, the same bytes: the generator is deterministic where the C side needs it
    assert (tmp_path / vectors.SECURE_HEADER).read_text() == (vectors.SDK_ROOT / vectors.SECURE_HEADER).read_text()
