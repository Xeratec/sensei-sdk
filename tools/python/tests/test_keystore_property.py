# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Property test of the collector side (design 1.9): random runs of uplinks, mixes, losses, reordering, duplicates,
forgeries, restarts and downlinks against a plain model of what the key store must accept."""

from __future__ import annotations

import os
import random

import pytest
from fs_helpers import NODE_ID, make_rig
from sensei_sdk import secure
from sensei_sdk.secure import KIND_DOWNLINK, KIND_UPLINK, GenerationError, KeyStore, ReplayError, SecureError


class Run:

    def __init__(self, tmp_path, seed: int) -> None:
        self.rng = random.Random(seed)
        self.rig = make_rig(tmp_path)
        self.store = self.rig.store
        self.node = self.rig.node
        self.inflight: list[tuple[bytes, bytes, int]] = []  # (envelope, plaintext, counter)
        self.seen: set[int] = set()  # counters opened
        self.highest = 0
        self.n = 0
        self.opened = self.refused = self.forged_refused = 0
        self.mixes = 0

    # the model: a counter is accepted once, if it is above highest - 64
    def expect_accept(self, counter: int) -> bool:
        return counter not in self.seen and counter > self.highest - 64

    def send(self) -> None:
        plain = os.urandom(self.rng.randrange(1, 40)) + b"%d" % self.n
        self.n += 1
        env = self.node.seal(KIND_UPLINK, plain)
        self.inflight.append((env, plain, secure.parse_header(env).counter))
        if self.rng.random() < 0.05:
            self.node.new_epoch()
        if self.rng.random() < 0.03:
            self.node.reboot()

    def deliver(self, env: bytes, plain: bytes | None, counter: int) -> None:
        want = self.expect_accept(counter)
        try:
            node_id, kind, got = secure.open_sealed(env, self.store)
        except ReplayError:
            assert not want, f"counter {counter} (highest {self.highest}) was refused"
            self.refused += 1
            return
        except GenerationError as exc:
            raise AssertionError(f"counter {counter} (highest {self.highest}): {exc}") from exc
        assert want and plain is not None and got == plain and node_id == NODE_ID
        self.seen.add(counter)
        self.highest = max(self.highest, counter)
        self.opened += 1

    def forge(self) -> None:
        h = self.rng.choice([self.highest + self.rng.randrange(-5, 400), self.rng.randrange(1, 2**32 - 1)])
        gen = self.rng.choice([self.node.gen, self.node.gen + 1, max(0, self.node.gen - 1)])
        env = secure.seal_raw(os.urandom(16), self.node.keys.key_id, max(1, h), KIND_UPLINK, b"forged", gen = gen)
        with pytest.raises(SecureError):
            secure.open_sealed(env, self.store)
        self.forged_refused += 1

    def mix_step(self) -> None:
        """Start a mix when the previous one is over, or go on with the open one: the node repeats its request (after a
        reboot), the collector answers the same way, the answer reaches the node or is lost."""
        if self.node.pending is None:
            st = self.store.node_state(NODE_ID)
            if st.prev_gen is not None or st.next_gen is not None:
                return
        req = self.node.rekey_request()
        if self.rng.random() < 0.5:
            self.node.reboot()
            assert self.node.rekey_request() == req
        ans = self.store.rekey_answer(NODE_ID, req)
        assert ans is not None and self.store.rekey_answer(NODE_ID, req) == ans
        if self.rng.random() < 0.6:  # else the answer is lost and the node asks again later
            assert self.node.rekey_answer(ans)
            self.mixes += 1

    def downlink(self) -> None:
        d = secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"down", self.store)
        # the node takes the generation of the store, or the one before it (until it asks for the next one)
        g_store, g_node = self.store.generation(NODE_ID), self.node.gen
        if g_store == g_node or (g_node == g_store + 1 and self.node.prev):
            assert self.node.open(d) == (KIND_DOWNLINK, b"down")

    def restart(self) -> None:
        self.store.flush()
        self.store = KeyStore.load(self.rig.path, responder = True, clock = self.rig.clock)

    def step(self) -> None:
        r = self.rng.random()
        if r < 0.45:
            self.send()
        elif r < 0.80 and self.inflight:
            i = self.rng.randrange(len(self.inflight))
            env, plain, counter = self.inflight[i]
            if self.rng.random() < 0.15:  # delivered twice
                self.deliver(env, plain, counter)
            else:
                self.inflight.pop(i)
            self.deliver(env, plain, counter)
        elif r < 0.84 and self.inflight:
            self.inflight.pop(self.rng.randrange(len(self.inflight)))  # lost
        elif r < 0.88:
            self.forge()
        elif r < 0.92:
            self.mix_step()
        elif r < 0.95:
            self.downlink()
        elif r < 0.96:
            self.restart()
        else:
            self.rig.clock.advance(self.rng.randrange(1, 4000))


@pytest.mark.parametrize("seed", range(12))
def test_collector_matches_the_model(tmp_path, seed):
    run = Run(tmp_path, seed)
    for _ in range(6000):
        run.step()
    run.restart()
    # the whole file comes back as it was: the next message opens
    run.deliver(*_fresh(run))
    gen = run.node.gen
    assert gen >= 3 and gen == run.mixes and run.store.generation(NODE_ID) in (gen, gen - 1)
    assert run.opened > 150 and run.forged_refused > 20


def _fresh(run: Run):
    plain = b"final"
    env = run.node.seal(KIND_UPLINK, plain)
    return env, plain, secure.parse_header(env).counter


def test_many_messages_and_mixes_total(tmp_path):
    """One long run: ten thousand uplinks over thirty mixes (counts for the report)."""
    run = Run(tmp_path, 99)
    mixes = 0
    for i in range(10_000):
        run.send()
        if run.rng.random() < 0.3:
            run.inflight.reverse() if run.rng.random() < 0.2 else None
        while run.inflight and run.rng.random() < 0.9:
            run.deliver(*run.inflight.pop(run.rng.randrange(len(run.inflight))))
        if i % 300 == 299:
            for env, plain, counter in run.inflight:
                run.deliver(env, plain, counter)
            run.inflight.clear()
            for _ in range(70):  # the window moves on
                run.send()
                run.deliver(*run.inflight.pop())
            req = run.node.rekey_request()
            ans = run.store.rekey_answer(NODE_ID, req)
            assert run.node.rekey_answer(ans)
            mixes += 1
    assert mixes >= 30 and run.node.gen == mixes and run.opened > 9000
