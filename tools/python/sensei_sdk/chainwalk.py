# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Cost control for walking an uplink chain on the collector.

A sealed uplink of a node in generation g >= 1 is opened with the key of its epoch. The collector keeps the chain key
at epoch `idx` only, so a message that is far ahead makes it walk the chain with one HMAC per epoch. Anyone can send a
frame with a far counter (the replay check passes, the tag fails after the walk), so the walk needs limits, and the
limits must not turn away a node that was silent for a long time:

1. Time bound. A node can start at most `rate` epochs per second (1, the design value) and one has `slack` epochs
   (4096) on top. A message may be at most `min(max_skip, slack + rate * (now - t))` epochs ahead of the last verified
   one, `t` being the time of that message. For a chain that was not used yet (the answered mix) the gap is measured
   from the chain head or from the epoch of the last verified message, whichever is later. A frame beyond is
   refused at once (`GapError`).
2. Cache. The keys passed on a walk are kept at checkpoints every 2^10 epochs, per chain, at most 1024 of them
   (32 kB of keys, 2^20 epochs). A checkpoint is only a key that the stored chain key can derive (the stored key is at
   least as old), so the cache gives away nothing the key store file does not have; a checkpoint behind the stored key is
   dropped as soon as the chain moves past it. The next far frame of that chain starts at a checkpoint: a flood of
   forged frames costs at most the first walk, then 1023 steps each. The caches of all chains share a least recently
   used limit.
3. Budget. Token buckets cap the steps per node (2^16 per minute) and for the whole store (2^19 per minute). A walk
   that does not fit goes as far as the budget allows (to a checkpoint) and refuses (`WalkBudgetError`); the next
   frame goes on from there, so a long gap opens after a few minutes even with a flood on the node. Short walks (at most
   64 steps, what traffic needs) are free.
4. The caller walks outside its lock (`Walker.walk` works on a copy of the stored key).
"""

from __future__ import annotations

import hashlib
import hmac
import threading
import time
import weakref
from collections import OrderedDict
from collections.abc import Callable
from dataclasses import dataclass, field

from .errors import EpochError, GapError, WalkBudgetError
from .ratchet import MAX_SKIP

SLACK_EPOCHS = 4096  # gap that is always allowed
RATE_EPOCHS_S = 1.0  # epochs a node can start per second
CHECKPOINT_SHIFT = 10  # a checkpoint every 2^10 epochs
CHECKPOINTS_MAX = 1 << (20 - CHECKPOINT_SHIFT)  # per chain: 1024 keys = 32 kB, the 2^20 epochs of MAX_SKIP
NODE_STEPS_MIN = 1 << 16  # budget per node and minute
NODE_BURST = 1 << 17
GLOBAL_STEPS_MIN = 1 << 19  # budget of the whole store per minute
GLOBAL_BURST = 1 << 20
FREE_STEPS = 64  # walks up to this are not charged: normal traffic needs one or two
CACHE_ENTRIES = 1 << 16  # all chains together: 2 MB of keys


@dataclass
class ChainState:
    """One stored uplink chain: the key at epoch `up_base + idx` (the epochs before it are gone) and the time of the last
    message that verified under it (`t`, unix seconds). `c1` and `gen` are for the key store (a previous chain ends
    before counter c1)."""

    gen: int
    up_base: int
    idx: int
    ck: bytes
    t: float = 0.0
    c1: int = 0
    cache: dict[int, bytes] = field(default_factory = dict, repr = False, compare = False)

    def copy(self) -> ChainState:
        return ChainState(self.gen, self.up_base, self.idx, self.ck, self.t, self.c1)


class _Bucket:
    """Token bucket in steps: `rate` per second up to `burst`."""

    def __init__(self, rate: float, burst: float, now: float) -> None:
        self.rate, self.burst, self.tokens, self.t = rate, burst, burst, now

    def _fill(self, now: float) -> None:
        if now > self.t:
            self.tokens = min(self.burst, self.tokens + (now - self.t) * self.rate)
        self.t = max(self.t, now)  # a clock that goes back does not refill

    def available(self, now: float) -> int:
        self._fill(now)
        return int(self.tokens)

    def take(self, n: int, now: float) -> None:
        self._fill(now)
        self.tokens = max(0.0, self.tokens - n)


@dataclass(frozen = True)
class Walked:
    """Chain key of the target epoch, and the one before it (None if the walk did not start before the target)."""

    ck: bytes
    ck_before: bytes | None
    steps: int


class Walker:
    """The limits above for all chains of one key store. Thread safe; the walks themselves take no lock."""

    def __init__(
        self,
        max_skip: int = MAX_SKIP,
        slack: int = SLACK_EPOCHS,
        rate: float = RATE_EPOCHS_S,
        node_steps_min: int = NODE_STEPS_MIN,
        node_burst: int = NODE_BURST,
        global_steps_min: int = GLOBAL_STEPS_MIN,
        global_burst: int = GLOBAL_BURST,
        cache_entries: int = CACHE_ENTRIES,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self.max_skip, self.slack, self.rate = max_skip, slack, rate
        self.node_rate, self.node_burst = node_steps_min / 60.0, node_burst
        self.cache_entries = cache_entries
        self.clock = clock
        self._lock = threading.Lock()
        self._global = _Bucket(global_steps_min / 60.0, global_burst, clock())
        self._nodes: dict[int, _Bucket] = {}
        self._lru: OrderedDict[int, weakref.ref[ChainState]] = OrderedDict()
        self._entries = 0
        self.steps_total = 0  # HMAC steps walked, for the tests and the metrics

    # ---- the cheap checks, before any walk ----

    def allowed_gap(self, t: float, now: float | None = None) -> int:
        """Epochs that a message may be ahead of the last verified one, `t` seconds ago: min(max_skip, slack + rate *
        (now - t))."""
        now = self.clock() if now is None else now
        return int(min(self.max_skip, self.slack + self.rate * max(0.0, now - t)))

    def precheck(self,
                 st: ChainState,
                 idx: int,
                 now: float | None = None,
                 base_idx: int | None = None,
                 ref_idx: int | None = None,
                 t_ref: float | None = None) -> None:
        """Refuse a target that is behind the stored key (EpochError) or too far ahead (GapError). No work.

        The hard ceiling (max_skip) counts from the stored key, `base_idx`. The time bound counts from `ref_idx`, the
        epoch of the last message that verified (default: the stored key), `t_ref` seconds ago (default: `st.t`). They
        differ for a chain that has not been used yet: its head is at up_base, but the first uplink of the new
        generation is at the next epoch boundary after the node switched, which can be far above it when the request
        waited long; the node has been sending in the old generation meanwhile (and the key store has seen that)."""
        base = st.idx if base_idx is None else base_idx
        ref = base if ref_idx is None else max(base, ref_idx)
        t = st.t if t_ref is None else t_ref
        if idx < base:
            raise EpochError(f"epoch index {idx} is behind the chain (at {base})")
        if idx - base > self.max_skip:
            raise GapError(f"epoch gap too large: {idx - base} > {self.max_skip}")
        allowed = self.allowed_gap(t, now)
        if idx - ref > allowed:
            raise GapError(f"epoch gap {idx - ref} is more than {allowed}, what the time since the last message of the "
                           "node allows")

    # ---- the walk ----

    def _checkpoint_for(self, st: ChainState, base_idx: int, target: int) -> tuple[int, bytes] | None:
        """The cached key at the highest checkpoint in (base_idx, target], if there is one."""
        k = (target >> CHECKPOINT_SHIFT) << CHECKPOINT_SHIFT
        with self._lock:
            while k > base_idx:
                ck = st.cache.get(k)
                if ck is not None:
                    self._touch(st)
                    return k, ck
                k -= 1 << CHECKPOINT_SHIFT
        return None

    def _store(self, st: ChainState, base_idx: int, found: dict[int, bytes]) -> None:
        if not found:
            return
        with self._lock:
            for k, ck in found.items():
                if k > max(st.idx, base_idx) and k not in st.cache and len(st.cache) < CHECKPOINTS_MAX:
                    st.cache[k] = ck
                    self._entries += 1
            self._touch(st)
            self._evict()

    def _touch(self, st: ChainState) -> None:  # the lock is held
        key = id(st)
        if key in self._lru:
            self._lru.move_to_end(key)
        else:
            self._lru[key] = weakref.ref(st, lambda _r, key = key: self._forget(key))

    def _forget(self, key: int) -> None:
        with self._lock:
            self._lru.pop(key, None)

    def _evict(self) -> None:  # the lock is held
        total = sum(len(r().cache) for r in self._lru.values() if r() is not None)
        self._entries = total
        while total > self.cache_entries and self._lru:
            _key, ref = self._lru.popitem(last = False)
            st = ref()
            if st is not None:
                total -= len(st.cache)
                st.cache.clear()
        self._entries = total

    def drop_below(self, st: ChainState, idx: int) -> None:
        """The chain moved to idx: the checkpoints at or behind it are not needed any more (and must not outlive the key
        they came from)."""
        with self._lock:
            old = [k for k in st.cache if k <= idx]
            for k in old:
                del st.cache[k]

    def cached(self, st: ChainState) -> int:
        with self._lock:
            return len(st.cache)

    def _bucket(self, node_id: int, now: float) -> _Bucket:
        with self._lock:
            b = self._nodes.get(node_id)
            if b is None:
                b = self._nodes[node_id] = _Bucket(self.node_rate, self.node_burst, now)
            return b

    def walk(self,
             node_id: int,
             st: ChainState,
             idx: int,
             base: tuple[int, bytes] | None = None,
             now: float | None = None,
             ref_idx: int | None = None,
             t_ref: float | None = None) -> Walked:
        """Chain key at index idx (and at idx - 1) from the stored key. `base` is (idx, key) as it was read under the lock
        of the caller (default: what `st` holds now); `st` is only the owner of the cache and is not changed (the caller
        commits the result under its lock). Starts at a cached checkpoint, caches the checkpoints it passes, and stays
        within the budgets: a walk that does not fit goes as far as a checkpoint within the budget and raises
        WalkBudgetError."""
        now = self.clock() if now is None else now
        base_idx, base_ck = base if base is not None else (st.idx, st.ck)
        self.precheck(st, idx, now, base_idx, ref_idx, t_ref)
        # stop one before the target to have the key before it (a commit keeps the epoch before the highest one)
        stop = max(base_idx, idx - 1)
        pos, ck = base_idx, base_ck
        cp = self._checkpoint_for(st, base_idx, stop)
        if cp is not None:
            pos, ck = cp
        need = idx - pos
        limit = idx
        if need > FREE_STEPS:
            node = self._bucket(node_id, now)
            with self._lock:
                avail = min(node.available(now), self._global.available(now))
            if need > avail:  # as far as the budget goes, to a checkpoint (what is walked past it would be lost)
                limit = ((pos + avail) >> CHECKPOINT_SHIFT) << CHECKPOINT_SHIFT
                limit = max(limit, pos)
        found: dict[int, bytes] = {}
        digest, sha = hmac.digest, hashlib.sha256
        before = ck if pos == idx - 1 else None
        mask = (1 << CHECKPOINT_SHIFT) - 1
        start = pos
        while pos < limit:
            end = min(limit, (pos | mask) + 1)  # the next checkpoint
            if pos < idx - 1 < end:  # the key before the target
                end = idx - 1
            for _ in range(end - pos):
                ck = digest(ck, b"\x02", sha)
            pos = end
            if pos == idx - 1:
                before = ck
            if pos & mask == 0 and pos < idx:
                found[pos] = ck
        walked = pos - start
        self.steps_total += walked
        if walked > FREE_STEPS:
            with self._lock:
                self._bucket_charge(node_id, walked, now)
        self._store(st, base_idx, found)
        if pos < idx:  # out of budget: the checkpoints stay, the next frame of the node goes on from there
            raise WalkBudgetError(f"walk budget used up (walked {walked} of {need} steps, the next frame goes on)")
        return Walked(ck, before, walked)

    def _bucket_charge(self, node_id: int, n: int, now: float) -> None:  # the lock is held
        b = self._nodes.get(node_id)
        if b is None:
            b = self._nodes[node_id] = _Bucket(self.node_rate, self.node_burst, now)
        b.take(n, now)
        self._global.take(n, now)

    def forget_node(self, node_id: int) -> None:
        with self._lock:
            self._nodes.pop(node_id, None)
