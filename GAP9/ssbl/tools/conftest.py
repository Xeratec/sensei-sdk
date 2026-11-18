# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""In CI ($CI is set, as GitHub Actions does) a skipped test is a failure: the signed-image tests skip without imgtool,
and CI must install it. Locally they skip as before."""

import os

import pytest


@pytest.hookimpl(hookwrapper = True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if rep.skipped and os.environ.get("CI") and not hasattr(rep, "wasxfail"):
        reason = rep.longrepr[2] if isinstance(rep.longrepr, tuple) else str(rep.longrepr)
        rep.outcome = "failed"
        rep.longrepr = f"skipped in CI (install what the test needs): {reason}"
