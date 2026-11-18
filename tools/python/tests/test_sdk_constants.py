# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""framing.py repeats constants of the header lib/sensei_link/include/sensei_link/sensei_link.h. The header is in
this repository (tests run from a checkout), so these tests always run."""

import re
from pathlib import Path

import pytest
from sensei_sdk import framing as fr

HEADER = "lib/sensei_link/include/sensei_link/sensei_link.h"
SDK_ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope = "module")
def header() -> str:
    return (SDK_ROOT / HEADER).read_text()


def define(text: str, name: str) -> int:
    m = re.search(rf"^#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)u?\b", text, re.M)
    assert m, f"{name} not defined in {HEADER}"
    return int(m.group(1), 0)


def enum_value(text: str, name: str) -> int:
    m = re.search(rf"^\s*{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)\b", text, re.M)
    assert m, f"{name} not in {HEADER}"
    return int(m.group(1), 0)


def test_version(header):
    assert fr.PROTO_VERSION == define(header, "SENSEI_LINK_VERSION")


def test_limits(header):
    assert fr.STREAM_MAX_MSG == define(header, "SENSEI_STREAM_MAX_MSG")
    assert fr.SPI_MAX_PAYLOAD == define(header, "SENSEI_SPI_MAX_PAYLOAD")
    assert fr.MTU_LORA == define(header, "SENSEI_MTU_LORA")
    assert fr.MTU_BLE == define(header, "SENSEI_MTU_BLE")
    assert fr.MTU_LTE == define(header, "SENSEI_MTU_LTE")


@pytest.mark.parametrize(
    "py, c",
    [
        ("CH_HOST_TO_NODE", "SENSEI_CH_HOST_TO_NODE"),
        ("CH_NODE_TO_HOST", "SENSEI_CH_NODE_TO_HOST"),
        ("CH_UPLINK", "SENSEI_CH_UPLINK"),
        ("CH_DOWNLINK", "SENSEI_CH_DOWNLINK"),
        ("CH_UPLINK_CHUNK", "SENSEI_CH_UPLINK_CHUNK"),
        ("CH_DOWNLINK_CHUNK", "SENSEI_CH_DOWNLINK_CHUNK"),
        ("CH_SEALED", "SENSEI_CH_SEALED"),
    ],
)
def test_channels(header, py, c):
    assert getattr(fr, py) == enum_value(header, c)


def test_every_sdk_channel_is_mirrored(header):
    in_header = set(re.findall(r"^\s*SENSEI_CH_([A-Z_]+)\s*=\s*0x", header, re.M))
    in_python = {n[3:] for n in dir(fr) if n.startswith("CH_")}
    assert in_header == in_python


@pytest.mark.parametrize(
    "py, c",
    [
        ("SPI_GAP9_JOB", "SENSEI_SPI_GAP9_JOB"),
        ("SPI_GAP9_RESULT", "SENSEI_SPI_GAP9_RESULT"),
        ("SPI_LPWAN_REQUEST", "SENSEI_SPI_LPWAN_REQUEST"),
        ("SPI_LPWAN_RESPONSE", "SENSEI_SPI_LPWAN_RESPONSE"),
        ("SPI_LPWAN_EVENT", "SENSEI_SPI_LPWAN_EVENT"),
    ],
)
def test_spi_types(header, py, c):
    assert getattr(fr, py) == enum_value(header, c)


def test_framing_sizes(header):
    root = SDK_ROOT / "lib/sensei_link/include/sensei_link"
    assert fr.FRAG_HEADER_SIZE == define((root / "frag.h").read_text(), "SENSEI_FRAG_HEADER_SIZE")
    assert fr.ACK_SIZE == define((root / "frag.h").read_text(), "SENSEI_FRAG_ACK_SIZE")
    spi = (root / "spi.h").read_text()
    assert fr.SPI_MAGIC == define(spi, "SENSEI_SPI_MAGIC")
    assert fr.SPI_HEADER_SIZE == define(spi, "SENSEI_SPI_HEADER_SIZE")
    assert fr.SPI_FLAG_ERROR == define(spi, "SENSEI_SPI_FLAG_ERROR")
    assert fr.SPI_FLAG_MORE == define(spi, "SENSEI_SPI_FLAG_MORE")


def test_secure_constants():
    from sensei_sdk import secure as sec

    text = (SDK_ROOT / "lib/sensei_secure/include/sensei_secure.h").read_text()
    assert sec.VERSION == define(text, "SENSEI_SECURE_VERSION")
    assert sec.MARKER == define(text, "SENSEI_SECURE_MARKER")
    assert sec.PUB_SIZE == define(text, "SENSEI_SECURE_PUB_SIZE")
    assert sec.TAG_SIZE == define(text, "SENSEI_SECURE_TAG_SIZE")
    assert sec.SENDER_SIZE == define(text, "SENSEI_SECURE_SENDER_SIZE")
    assert sec.WINDOW == define(text, "SENSEI_SECURE_WINDOW")
    assert sec.MAX_SENDERS == define(text, "SENSEI_SECURE_MAX_SENDERS")
    assert sec.OVERHEAD == 26 and sec.OVERHEAD_NODE_ID == 34 and sec.OVERHEAD_DOWN == 30
    for py, c in (
        ("KIND_UPLINK", "SENSEI_SECURE_KIND_UPLINK"),
        ("KIND_DOWNLINK", "SENSEI_SECURE_KIND_DOWNLINK"),
        ("KIND_NODE_TO_HOST", "SENSEI_SECURE_KIND_NODE_TO_HOST"),
        ("KIND_SETTINGS", "SENSEI_SECURE_KIND_SETTINGS"),
    ):
        assert getattr(sec, py) == enum_value(text, c)
