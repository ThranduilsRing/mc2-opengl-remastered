"""Offline tests for tools/tree_hash.py (the baseline check skips when the srcdata roots are absent)."""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tree_hash  # noqa: E402


def _rec(t, x):
    return struct.pack("<i3ffiiI2i", t, x, 0.0, 0.0, 0.0, 0, 0, 0, 0, 0)


def test_hash_records_only_tree_types_in_order():
    pkt = struct.pack("<I", 3) + _rec(5, 1.0) + _rec(9, 2.0) + _rec(5, 3.0)
    a = tree_hash.hash_records(pkt, {5})
    assert a["trees"] == 2 and a["objects"] == 3
    swapped = struct.pack("<I", 3) + _rec(5, 3.0) + _rec(9, 2.0) + _rec(5, 1.0)
    assert tree_hash.hash_records(swapped, {5})["sha256"] != a["sha256"]
    moved = struct.pack("<I", 3) + _rec(5, 1.0) + _rec(9, 7.0) + _rec(5, 3.0)
    assert tree_hash.hash_records(moved, {5})["sha256"] == a["sha256"]


def test_bad_size():
    assert tree_hash.hash_records(struct.pack("<I", 2) + b"x", set())["error"] == "bad_size"


@pytest.mark.skipif(not Path(tree_hash.ROOTS["stock"]).exists(), reason="stock srcdata absent")
def test_baseline_matches():
    base = json.loads((Path(__file__).parent / "tree_hash_baseline.json").read_text())
    cur = tree_hash.scan_all(tree_hash.ROOTS)
    for name, b in base.items():
        if name in cur:
            assert cur[name] == b, name
