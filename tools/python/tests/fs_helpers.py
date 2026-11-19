# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Shared set-up of the forward secrecy tests of the key store: a collector store, a node twin and a fake clock."""

from __future__ import annotations

from dataclasses import dataclass

from sensei_sdk import secure
from sensei_sdk.chainwalk import Walker
from sensei_sdk.secure import KeyStore, NodeSession

NODE_ID = 0x0123456789ABCDEF


class Clock:
    """A clock that only moves when the test says so."""

    def __init__(self, t: float = 1_800_000_000.0) -> None:
        self.t = t

    def __call__(self) -> float:
        return self.t

    def advance(self, s: float) -> None:
        self.t += s


@dataclass
class Rig:
    store: KeyStore
    node: NodeSession
    pem: object
    clock: Clock
    path: object
    node_priv: bytes = b""

    def up(self, plain: bytes = b"x", kind: int = secure.KIND_UPLINK) -> bytes:
        return self.node.seal(kind, plain)

    def mix(self) -> secure.RekeyAnswer:
        """A whole mix, no loss: request, answer (saved), the node switches."""
        req = self.node.rekey_request()
        ans = self.store.rekey_answer(NODE_ID, req)
        assert ans is not None and self.node.rekey_answer(ans)
        return ans

    def promote(self, plain: bytes = b"first") -> secure.Opened:
        """The first uplink of the new generation, opened by the store."""
        o = secure.open_sealed_ex(self.up(plain), self.store)
        assert o.promoted
        return o


def make_rig(tmp_path, responder: bool = True, name: str = "keys.json", clock: Clock | None = None, **kw) -> Rig:
    clock = clock or Clock()
    pem = tmp_path / "backend.pem"
    if not pem.exists():
        secure.backend_keygen(pem)
    path = tmp_path / name
    store = KeyStore.load(path, private_pem = pem, responder = responder, clock = clock, **kw)
    priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    node = NodeSession(priv, store.backend_public_key())
    return Rig(store, node, pem, clock, path, priv)


def slack_walker(clock: Clock, **kw) -> Walker:
    """A walker without the limits of the time bound and the budgets: what the code before Decision M2 did."""
    return Walker(slack = 1 << 20,
                  node_steps_min = 1 << 40,
                  node_burst = 1 << 40,
                  global_steps_min = 1 << 40,
                  global_burst = 1 << 40,
                  clock = clock,
                  **kw)
