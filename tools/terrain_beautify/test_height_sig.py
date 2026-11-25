#!/usr/bin/env python3
"""TERRAIN-BEAUTY-SIG-1: the gameplay-height signature binds a bake to one grid."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from height_sig import height_sig  # noqa: E402


def grid(side=100):
    r = np.random.RandomState(7)
    return (r.rand(side, side) * 800.0).astype(np.float32)


def test_match():
    g = grid()
    assert height_sig(g) == height_sig(g.copy())


def test_one_cell_edit_rejected():
    g = grid()
    e = g.copy()
    e[41, 17] += 1.0
    assert height_sig(g) != height_sig(e)


def test_size_mismatch_rejected():
    assert height_sig(grid(100)) != height_sig(grid(120))
    assert height_sig(grid(100)).split()[0] == "100"


def test_known_vector():
    # pins the byte layout the C++ side hashes: u32 side, then float32 LE rows
    assert height_sig(np.zeros((2, 2), dtype=np.float32)) == "2 " + format(
        __import__("height_sig").fnv1a64(b"\x02\x00\x00\x00" + b"\x00" * 16), "016x")

def test_cpp_fixture():
    elev = np.array([[0, 1.5, -2.25], [100, 3.75, 0.125], [7, 8, 9.5]])
    assert height_sig(elev) == "3 08878b3b0a6edae3"
