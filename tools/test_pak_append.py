"""Offline round-trip tests for tools/pak_append.py.

Builds a small synthetic .pak in-process (no dependency on the deployed
object2.pak), appends/replaces a packet, then re-reads it with pak_dump's
independent parser to prove the on-disk format stays readable.
Also does an append round-trip against the real deployed object2.pak
(skipped if not present) to prove existing packets survive byte-for-byte.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pak_append  # noqa: E402
import pak_dump  # noqa: E402

OBJECT2_PAK = Path(
    "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0/data/objects/object2.pak"
)


def _make_synthetic_pak(tmp_path: Path) -> Path:
    """Build a minimal valid PacketFile with 2 packets (RAW + ZLIB) using
    pak_append's own writer, then hand back the path."""
    packets = [
        pak_append.PacketRecord(0, pak_append.ST_RAW, b"hello world"),
        pak_append.PacketRecord(1, pak_append.ST_ZLIB, pak_append.make_zlib_payload(b"FITini\nfoo=1\nFITend")),
    ]
    out = tmp_path / "synthetic.pak"
    pak_append.write_pak(packets, out)
    return out


def test_synthetic_roundtrip_readback(tmp_path):
    pak_path = _make_synthetic_pak(tmp_path)
    packets = pak_dump.read_packets(pak_path)
    assert len(packets) == 2
    assert pak_dump.decode_packet(packets[0]) == b"hello world"
    assert pak_dump.decode_packet(packets[1]) == b"FITini\nfoo=1\nFITend"


def test_append_new_packet(tmp_path):
    pak_path = _make_synthetic_pak(tmp_path)
    new_text = tmp_path / "new.txt"
    new_text.write_bytes(b"FITini\nnew=1\nFITend")

    class Args:
        pak = str(pak_path)
        text_file = str(new_text)
        bytes_literal = None
        out = None

    rc = pak_append.cmd_append(Args())
    assert rc == 0

    packets = pak_dump.read_packets(pak_path)
    assert len(packets) == 3
    # original 2 packets survive unchanged
    assert pak_dump.decode_packet(packets[0]) == b"hello world"
    assert pak_dump.decode_packet(packets[1]) == b"FITini\nfoo=1\nFITend"
    # new packet appended at index 2
    assert pak_dump.decode_packet(packets[2]) == b"FITini\nnew=1\nFITend"
    assert packets[2].storage_type == pak_dump.ST_ZLIB


def test_replace_existing_packet(tmp_path):
    pak_path = _make_synthetic_pak(tmp_path)
    new_text = tmp_path / "replacement.txt"
    new_text.write_bytes(b"FITini\nreplaced=1\nFITend")

    class Args:
        pak = str(pak_path)
        packet = 1
        text_file = str(new_text)
        bytes_literal = None
        out = None

    rc = pak_append.cmd_replace(Args())
    assert rc == 0

    packets = pak_dump.read_packets(pak_path)
    assert len(packets) == 2  # count unchanged
    assert pak_dump.decode_packet(packets[0]) == b"hello world"
    assert pak_dump.decode_packet(packets[1]) == b"FITini\nreplaced=1\nFITend"


@pytest.mark.skipif(not OBJECT2_PAK.exists(), reason="deployed object2.pak not present")
def test_append_against_real_object2_pak_on_a_copy(tmp_path):
    copy_path = tmp_path / "object2_copy.pak"
    shutil.copyfile(OBJECT2_PAK, copy_path)

    orig_packets = pak_dump.read_packets(OBJECT2_PAK)
    orig_count = len(orig_packets)
    # sample a few known packets to compare after round-trip
    sample_indices = [0, 289, 335, orig_count - 1]
    orig_decoded = {i: pak_dump.decode_packet(orig_packets[i]) for i in sample_indices}

    new_text = tmp_path / "cliff_fit.txt"
    new_text.write_bytes(
        b"FITini \n\n[ObjectClass]\nl ObjectTypeNum=1\n\n"
        b"[ObjectType]\nst Name=\"MarbleCliff\"\nst AppearanceName=\"MarbleCliff\"\n"
        b"l ExplosionObject=0\nl DestroyedObject=0\nf ExtentRadius=35.000000\n\n"
        b"[BuildingData]\nul DmgLevel=0\nl SetImpassable=0\nb blockLineOfFire=FALSE\n"
        b"FITend"
    )

    class Args:
        pak = str(copy_path)
        text_file = str(new_text)
        bytes_literal = None
        out = None

    rc = pak_append.cmd_append(Args())
    assert rc == 0

    new_packets = pak_dump.read_packets(copy_path)
    assert len(new_packets) == orig_count + 1
    for i in sample_indices:
        assert pak_dump.decode_packet(new_packets[i]) == orig_decoded[i], f"packet {i} changed!"
    new_decoded = pak_dump.decode_packet(new_packets[orig_count])
    assert new_decoded is not None
    assert b"MarbleCliff" in new_decoded
