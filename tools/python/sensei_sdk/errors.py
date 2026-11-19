# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Errors of the end-to-end envelope, shared by `secure`, `ratchet` and `chainwalk` (`secure` re-exports them)."""

from __future__ import annotations


class SecureError(Exception):
    """Authentication, replay, format or key error."""


class UnknownKeyError(SecureError):
    pass


class ReplayError(SecureError):
    pass


class GenerationError(SecureError):
    """No key for the generation of an envelope (for example a ratchet envelope of a node that is ahead of the key
    store: it has to be provisioned again)."""


class EpochError(SecureError):
    """The epoch of a counter is before the start of its chain, or behind the part of the chain that is kept."""


class GapError(SecureError):
    """The chain would have to be walked further than the collector allows: more than SENSEI_E2E_MAX_SKIP epochs, or more
    than the time since the last message of the node explains."""


class WalkBudgetError(GapError):
    """The walk budget of the node or of the whole key store is used up (the walk goes on at the next message)."""
