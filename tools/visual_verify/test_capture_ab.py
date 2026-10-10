"""What: tests for capture_ab.py - the compare step reports the noise floor first, identical frames read as zero, a changed block is found, a missing set is named.
Why: a refactor is judged by comparing its diff to the run-to-run noise; a compare that silently skipped a missing set would read as 'no change'."""
import os, sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_ab

BM = "mc2_01_saved_205644"


def put(root, set_name, arr):
    d = root / set_name / BM
    d.mkdir(parents=True)
    Image.fromarray(arr).save(d / (BM + ".png"))


def sets(tmp_path, changed):
    base = np.zeros((64, 64, 3), np.uint8)
    other = base.copy()
    if changed:
        other[10:30, 10:30] = 200
    for s in ("b1-gl", "b2-gl", "b-vk"):
        put(tmp_path, s, base)
    for s in ("a1-gl", "a2-gl", "a-vk"):
        put(tmp_path, s, other)


def test_identical_sets_read_as_zero(tmp_path):
    sets(tmp_path, False)
    lines = capture_ab.compare(str(tmp_path), "b", "a", [BM])
    assert len(lines) == 4 and lines[0].split()[1] == "NOISE"
    assert all("outside_box=0 clusters=0" in l for l in lines)


def test_changed_block_is_found_but_noise_stays_zero(tmp_path):
    sets(tmp_path, True)
    lines = capture_ab.compare(str(tmp_path), "b", "a", [BM])
    assert "outside_box=0" in lines[0]
    assert all("outside_box=400" in l and "max_cluster=20x20@10,10" in l for l in lines[1:])


def test_missing_set_is_named(tmp_path):
    sets(tmp_path, False)
    lines = capture_ab.compare(str(tmp_path), "b", "nope", [BM])
    assert any("MISSING" in l for l in lines[1:])
