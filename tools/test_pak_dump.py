"""Offline tests for tools/pak_dump.py -- skip-if-missing against the deployed
object2.pak (not part of the repo, lives in the deploy tree)."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pak_dump  # noqa: E402

OBJECT2_PAK = Path(
    "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0/data/objects/object2.pak"
)


def test_lzw_decompress_roundtrip_empty():
    # Degenerate input: EOF code only. Should not raise, returns empty bytes.
    # code_width starts at 9 bits; HASH_EOF=257 encoded LSB-first in the first 9 bits.
    eof_code = 257
    val = eof_code
    b = bytes([val & 0xFF, (val >> 8) & 0xFF, 0, 0])
    assert pak_dump.lzw_decompress(b) == b""


@pytest.mark.skipif(not OBJECT2_PAK.exists(), reason="deployed object2.pak not present")
def test_list_header_sanity():
    packets = pak_dump.read_packets(OBJECT2_PAK)
    assert len(packets) > 0
    # every packet must have a recognized storage type
    for rec in packets:
        assert rec.storage_type in pak_dump.STORAGE_NAMES


@pytest.mark.skipif(not OBJECT2_PAK.exists(), reason="deployed object2.pak not present")
def test_find_fit_blocks_line_of_fire():
    packets = pak_dump.read_packets(OBJECT2_PAK)
    needle = b"BlocksLineOfFire"
    hits = []
    for rec in packets:
        dec = pak_dump.decode_packet(rec)
        if dec and needle in dec:
            hits.append(rec.index)
    assert hits, "expected at least one packet containing BlocksLineOfFire"


@pytest.mark.skipif(not OBJECT2_PAK.exists(), reason="deployed object2.pak not present")
def test_dump_known_building_packet():
    packets = pak_dump.read_packets(OBJECT2_PAK)
    rec = packets[663]
    dec = pak_dump.decode_packet(rec)
    assert dec is not None
    assert b"FITini" in dec
    assert b"BlocksLineOfFire" in dec
