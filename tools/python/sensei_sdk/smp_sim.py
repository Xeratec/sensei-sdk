# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Simulated SMP server (MCUboot with mcumgr) for tests, plus a builder for fake MCUboot images.

Two image slots. Upload fills slot 1, `test` marks it pending, a reset swaps it in unconfirmed, and
`confirm` makes it permanent. Without a confirm the next reset reverts to the old image, as MCUboot does.

What MCUboot does at the reset, before the swap: an image in slot 1 that is not signed with the trusted key is
erased, and so is one with a lower version than the running one (downgrade prevention, `downgrade_prevention`).
The node keeps running the old image then.

What the Zephyr image group does at an upload (first chunk): with `upgrade` set, an image whose version is not
higher than the running one is refused (rc 27); an image larger than the slot minus the swap footer is refused
(rc 30, `too_large_check`, CONFIG_MCUMGR_GRP_IMG_TOO_LARGE_SYSBUILD); and while the running image is on trial or a
test image is pending, slot 1 is in use (rc 9, "no free slot"). These are SMP v2 errors of the image group.

What the firmware does on a trial image (SDK `sensei_ota`): confirm it after `auto_confirm_s` if
`healthy`, and reset after `trial_timeout_s` if it is still unconfirmed (MCUboot reverts). Both are wall-clock
seconds here; `auto_confirm_s=None` leaves the confirm to the host. With `needs_link` (Release) the node confirms
only after it has seen an SMP request on the trial image (a frame of the node's own protocol does not count).

`recovery=True` plays MCUboot's serial recovery instead of the app: uploads go into slot 0 and the next reset
runs that image.

Failure hooks: `drop_next` (no reply to the next N requests), `reject_upload_after` (error after N bytes),
`corrupt` (flip a byte of the stored image, so the hash check at the end fails).
"""

from __future__ import annotations

import asyncio
import hashlib
import re
import struct
from collections.abc import Callable
from dataclasses import dataclass

from . import smp

TLV_ECDSA_SIG = 0x22
IMG_ERR_NO_FREE_SLOT = 9  # image group return codes (zephyr img_mgmt_errors.h)
IMG_ERR_VERSION_NOT_HIGHER = 27
IMG_ERR_TOO_LARGE = 30


def make_image(
    version: tuple[int, int, int, int] = (1, 0, 0, 0),
    size: int = 4000,
    seed: int = 0,
    key: int = 0,
    kind: str | None = None,
) -> bytes:
    """An MCUboot-style image: 32 B header, body, TLV area with the SHA-256 and a fake signature.
    `key` stands for the signing key: the simulated MCUboot trusts one key (`SimSmpServer.trusted_key`).
    `kind`: the board TLV (protected, as imgtool --custom-tlv 0xa0 makes it), None: no such TLV."""
    hdr_size = 32
    body = bytes((seed * 31 + i * 7 + (i >> 8)) & 0xFF for i in range(size))
    prot = b""
    if kind is not None:
        value = kind.encode()
        prot_tlv = struct.pack("<HH", smp.TLV_KIND, len(value)) + value
        prot = struct.pack("<HH", smp.TLV_PROT_INFO_MAGIC, 4 + len(prot_tlv)) + prot_tlv
    header = struct.pack("<IIHHII BBHI I", smp.IMAGE_MAGIC, 0, hdr_size, len(prot), size, 0, *version,
                         0)  # last I pads the header to 32 B
    digest = hashlib.sha256(header + body + prot).digest()  # the hash covers the protected TLVs
    sig = hashlib.sha256(struct.pack("<I", key) + digest).digest() * 2  # 64 B, like ECDSA P-256
    tlv = struct.pack("<BxH", smp.TLV_SHA256, 32) + digest + struct.pack("<BxH", TLV_ECDSA_SIG, 64) + sig
    info = struct.pack("<HH", smp.TLV_INFO_MAGIC, 4 + len(tlv)) + tlv
    return header + body + prot + info


def _signed_with(image: bytes, key: int) -> bool:
    info = smp.parse_image(image)
    sig = hashlib.sha256(struct.pack("<I", key) + info.hash).digest() * 2
    return image.endswith(sig)


@dataclass
class _Slot:
    image: bytes
    pending: bool = False
    permanent: bool = False
    confirmed: bool = False

    @property
    def info(self) -> smp.ImageInfo:
        return smp.parse_image(self.image)


class _PacketPort:
    """Packet-mode access (as the BLE characteristic): requests in, responses out in small fragments."""

    def __init__(self, server: SimSmpServer, fragment: int = 23) -> None:
        self.server, self.fragment = server, fragment
        self._feed: Callable[[bytes], None] | None = None

    def attach(self, feed: Callable[[bytes], None]):
        self._feed = feed
        return lambda: setattr(self, "_feed", None)

    async def write(self, data: bytes) -> None:
        for rsp in self.server.handle_packet(data):
            if self._feed is not None:
                for i in range(0, len(rsp), self.fragment):
                    self._feed(rsp[i:i + self.fragment])


class SimSmpServer:

    def __init__(
        self,
        active: bytes | None = None,
        slot_size: int = 1 << 20,
        on_reset: Callable[[], None] | None = None,
        trusted_key: int = 0,
        downgrade_prevention: bool = True,
        auto_confirm_s: float | None = 0.05,
        trial_timeout_s: float = 0.5,
        healthy: bool = True,
        recovery: bool = False,
        needs_link: bool = False,
        too_large_check: bool = True,
        footer_size: int = 4096,
    ):
        self.slots = [_Slot(active or make_image(), confirmed = True), None]
        self.slot_size = slot_size
        self.on_reset = on_reset or (lambda: None)
        self.trusted_key = trusted_key
        self.downgrade_prevention = downgrade_prevention
        self.auto_confirm_s = auto_confirm_s
        self.trial_timeout_s = trial_timeout_s
        self.healthy = healthy
        self.recovery = recovery
        self.needs_link = needs_link
        self.link_seen = False
        self.too_large_check = too_large_check
        self.footer_size = footer_size  # swap trailer: an image has to leave room for it in the slot
        self.refused: list[str] = []  # why MCUboot dropped a pending image at a reset
        self.reverts = 0
        self.confirmed_by_node = 0
        self._timers: list[asyncio.TimerHandle] = []
        self.reset_count = 0
        self.boot_modes: list[int | None] = []  # boot_mode field of each OS reset request
        self.recovery_requested = False  # the last reset asked for the bootloader's serial recovery
        self.drop_next = 0
        self.reject_upload_after: int | None = None
        self.corrupt = False
        self.n_requests = 0
        self.upload_chunks = 0
        self._rx = bytearray()
        self._rx_total = 0
        self._rx_sha = b""
        self._serial = smp.SerialDecoder()
        self._line_re = re.compile(rb"(?:\x06\x09|\x04\x14)[A-Za-z0-9+/=]*\n")

    # --- state seen by the node ------------------------------------------------------------------
    @property
    def active(self) -> smp.ImageInfo:
        return self.slots[0].info

    def _state_body(self) -> dict:
        imgs = []
        for n, sl in enumerate(self.slots):
            if sl is None:
                continue
            info = sl.info
            imgs.append({
                "image": 0,
                "slot": n,
                "version": info.version_str,
                "hash": info.hash,
                "bootable": True,
                "pending": sl.pending,
                "confirmed": sl.confirmed,
                "active": n == 0,
                "permanent": sl.permanent,
            })
        return {"images": imgs}

    @property
    def image_id(self) -> int:
        """Id of the running image (first 4 bytes of its hash, little endian): what a device reports in its status."""
        return int.from_bytes(self.slots[0].info.hash[:4], "little")

    @property
    def running_confirmed(self) -> bool:
        """False while the running image is on trial."""
        return self.slots[0].confirmed

    def _bootloader(self) -> None:
        """What MCUboot decides at a reset."""
        for t in self._timers:
            t.cancel()
        self._timers = []
        sec = self.slots[1]
        if sec is not None and sec.pending:
            reason = None
            if not _signed_with(sec.image, self.trusted_key):
                reason = "bad signature"
            elif self.downgrade_prevention and sec.info.version3 < self.slots[0].info.version3:
                reason = "downgrade prevention"
            elif len(sec.image) > self.slot_size - self.footer_size:
                reason = "image too large for the swap"
            if reason is not None:
                self.refused.append(reason)
                self.slots[1] = None  # erased, the old image keeps running
                return
            # swap: the new image runs, unconfirmed unless it was marked permanent
            old = self.slots[0]
            sec.pending = False
            sec.confirmed = sec.permanent
            old.confirmed = False
            self.slots = [sec, old]
        elif not self.slots[0].confirmed and self.slots[1] is not None:
            # trial image was not confirmed: revert
            self.slots = [self.slots[1], self.slots[0]]
            self.slots[0].confirmed = True
            self.slots[0].pending = False
            self.reverts += 1

    def _do_reset(self) -> None:
        self.reset_count += 1
        if self.recovery:
            self.slots[0].confirmed = True  # the recovery image runs as it is
        else:
            self._bootloader()
        self.on_reset()
        self._start_trial()

    def _start_trial(self) -> None:
        """The firmware of a trial image: confirm after the check, reset if that does not happen."""
        self.link_seen = False  # RAM flag of the running image
        if self.recovery or self.slots[0].confirmed:
            return
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return
        if self.auto_confirm_s is not None and self.healthy:
            self._timers.append(loop.call_later(self.auto_confirm_s, self._auto_confirm))
        self._timers.append(loop.call_later(self.trial_timeout_s, self._trial_expired))

    def _auto_confirm(self) -> None:
        if self.slots[0].confirmed:
            return
        if self.needs_link and not self.link_seen:  # the node checks again later
            self._timers.append(asyncio.get_running_loop().call_later(self.auto_confirm_s, self._auto_confirm))
            return
        self.slots[0].confirmed = True
        self.confirmed_by_node += 1

    def _trial_expired(self) -> None:
        if not self.slots[0].confirmed:
            self._do_reset()  # the node resets itself, MCUboot reverts

    # --- transports ------------------------------------------------------------------------------
    def packet_port(self, fragment: int = 23) -> _PacketPort:
        return _PacketPort(self, fragment)

    def extract_serial(self, data: bytes) -> tuple[bytes, list[bytes]]:
        """Split SMP serial lines out of stream bytes. Returns (other bytes, response line blocks)."""
        out = []
        for m in self._line_re.finditer(data):
            for pkt in self._serial.feed(m.group(0)):
                out.extend(smp.serial_encode(r) for r in self.handle_packet(pkt))
        return self._line_re.sub(b"", data), out

    # --- requests --------------------------------------------------------------------------------
    def handle_packet(self, data: bytes) -> list[bytes]:
        try:
            req = smp.decode_packet(data)
        except smp.SmpError:
            return []
        self.n_requests += 1
        self.link_seen = True
        if self.drop_next > 0:
            self.drop_next -= 1
            return []
        body, after = self._dispatch(req)
        rsp = smp.encode_packet(req.op + 1, req.group, req.cmd, req.seq, body)
        if after:
            after()
        return [rsp]

    def _dispatch(self, req: smp.Packet):
        g, c, op, b = req.group, req.cmd, req.op, req.body
        if g == smp.GROUP_OS and c == smp.OS_ECHO and op == smp.OP_WRITE:
            return {"r": b.get("d", "")}, None
        if g == smp.GROUP_OS and c == smp.OS_RESET and op == smp.OP_WRITE:
            mode = b.get("boot_mode")
            self.boot_modes.append(mode)
            self.recovery_requested = mode == smp.BOOT_MODE_BOOTLOADER
            return {}, self._do_reset
        if g == smp.GROUP_IMAGE and c == smp.IMG_STATE:
            if op == smp.OP_READ:
                return self._state_body(), None
            return self._set_state(b), None
        if g == smp.GROUP_IMAGE and c == smp.IMG_UPLOAD and op == smp.OP_WRITE:
            return self._upload(b), None
        if g == smp.GROUP_IMAGE and c == smp.IMG_ERASE and op == smp.OP_WRITE:
            self.slots[1] = None
            return {}, None
        return {"rc": 8}, None  # MGMT_ERR_ENOTSUP

    def _set_state(self, b: dict) -> dict:
        h, confirm = b.get("hash"), bool(b.get("confirm", False))
        if h is None:
            if not confirm:
                return {"rc": smp.MGMT_ERR_EINVAL}
            self.slots[0].confirmed = True  # confirm the running image
            return self._state_body()
        for n, sl in enumerate(self.slots):
            if sl is not None and sl.info.hash == bytes(h):
                if n == 0:
                    if confirm:
                        sl.confirmed = True
                    return self._state_body()
                sl.pending, sl.permanent = True, confirm
                return self._state_body()
        return {"rc": smp.MGMT_ERR_ENOENT}

    @staticmethod
    def _img_err(rc: int) -> dict:
        return {"err": {"group": smp.GROUP_IMAGE, "rc": rc}}

    def _upload(self, b: dict) -> dict:
        off = int(b.get("off", -1))
        data = bytes(b.get("data", b""))
        if off == 0:
            if "len" not in b:
                return {"rc": smp.MGMT_ERR_EINVAL}
            self._rx = bytearray()
            self._rx_total = int(b["len"])
            self._rx_sha = bytes(b.get("sha", b""))
            limit = self.slot_size - (self.footer_size if self.too_large_check else 0)
            if self._rx_total > limit:
                return self._img_err(IMG_ERR_TOO_LARGE)
            if not self.recovery:
                sec = self.slots[1]
                if not self.slots[0].confirmed or (sec is not None and sec.pending):
                    return self._img_err(IMG_ERR_NO_FREE_SLOT)  # slot 1 holds the revert image or a test image
            if b.get("upgrade") and len(data) >= 24:  # the first chunk holds the image header
                if struct.unpack_from("<BBH", data, 20) <= self.slots[0].info.version3:
                    return self._img_err(IMG_ERR_VERSION_NOT_HIGHER)
        if off != len(self._rx):
            return {"rc": 0, "off": len(self._rx)}  # resync, as Zephyr does
        if self.reject_upload_after is not None and len(self._rx) >= self.reject_upload_after:
            return {"rc": 2}  # MGMT_ERR_ENOMEM
        self._rx += data
        self.upload_chunks += 1
        if len(self._rx) >= self._rx_total:
            image = bytes(self._rx[:self._rx_total])
            if self.corrupt:
                image = image[:100] + bytes([image[100] ^ 1]) + image[101:]
            try:
                info = smp.parse_image(image)
                body = image[:32 + struct.unpack_from("<I", image, 12)[0]]
                if hashlib.sha256(body).digest() != info.hash:
                    raise smp.SmpError("hash mismatch")
                if self._rx_sha and hashlib.sha256(image).digest()[:len(self._rx_sha)] != self._rx_sha:
                    raise smp.SmpError("sha mismatch")
            except smp.SmpError:
                self._rx = bytearray()
                return {"rc": smp.MGMT_ERR_EINVAL}
            if self.recovery:
                self.slots[0] = _Slot(image, confirmed = True)  # serial recovery writes the primary slot
            else:
                self.slots[1] = _Slot(image)
        return {"rc": 0, "off": len(self._rx)}
