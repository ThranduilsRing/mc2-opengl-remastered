"""Tests for tools/visual_verify/montage_compact.py (synthetic PNGs; Pillow + numpy only).

Run:
  py -3 -B -m pytest tools/test_montage_compact.py -q -p no:cacheprovider
"""
import importlib.util
import os
import re
import sys

import numpy as np
import pytest
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "montage_compact", os.path.join(HERE, "visual_verify", "montage_compact.py"))
mc = importlib.util.module_from_spec(_spec)
sys.modules["montage_compact"] = mc
_spec.loader.exec_module(mc)

W, H = 400, 200
LINE = re.compile(r"^DIFF outside_box=(\d+) clusters=(\d+) max_cluster=(\d+)x(\d+)@(\d+),(\d+) inside_box=(\d+)$")


def frame(value=100):
    return np.full((H, W, 3), value, dtype=np.uint8)


def save(arr, path):
    Image.fromarray(arr).save(path)
    return str(path)


def patch(arr, x, y, w, h, value=200):
    out = arr.copy()
    out[y:y + h, x:x + w] = value
    return out


def run(tmp_path, capsys, before, after, *extra):
    b, a = save(before, tmp_path / "b.png"), save(after, tmp_path / "a.png")
    out = str(tmp_path / "out.png")
    rc = mc.main([b, a, "--out", out, *extra])
    text = capsys.readouterr().out.strip()
    return rc, text, out


def fields(text):
    m = LINE.match(text)
    assert m, text
    return tuple(int(g) for g in m.groups())


def test_identical_images(tmp_path, capsys):
    rc, text, out = run(tmp_path, capsys, frame(), frame())
    assert rc == 0
    assert fields(text) == (0, 0, 0, 0, 0, 0, 0)
    assert Image.open(out).size == (W // 4, H // 4)


def test_small_patch_is_one_cluster_with_exact_box(tmp_path, capsys):
    rc, text, out = run(tmp_path, capsys, frame(), patch(frame(), 120, 60, 10, 10))
    assert rc == 0
    assert fields(text) == (100, 1, 10, 10, 120, 60, 0)
    im = np.asarray(Image.open(out).convert("RGB"))
    assert tuple(im[60 // 4 - 1, 120 // 4 - 1]) == (255, 0, 0)  # box corner at 0.25 scale


def test_patch_inside_mech_box_ignored(tmp_path, capsys):
    rc, text, _ = run(tmp_path, capsys, frame(), patch(frame(), 120, 60, 10, 10), "--mech-box", "100,40,60,60")
    assert rc == 0
    assert fields(text) == (0, 0, 0, 0, 0, 0, 100)


def test_patch_straddling_mech_box_is_kept(tmp_path, capsys):
    _, text, _ = run(tmp_path, capsys, frame(), patch(frame(), 150, 60, 20, 10), "--mech-box", "100,40,60,60")
    assert fields(text) == (200, 1, 20, 10, 150, 60, 0)


def test_patch_outside_mech_box_counted_inside_one_ignored(tmp_path, capsys):
    after = patch(patch(frame(), 120, 60, 10, 10), 300, 150, 5, 5)
    _, text, _ = run(tmp_path, capsys, frame(), after, "--mech-box", "100,40,60,60")
    assert fields(text) == (25, 1, 5, 5, 300, 150, 100)


def test_nearby_patches_merge_distant_ones_do_not(tmp_path, capsys):
    near = patch(patch(frame(), 50, 50, 5, 5), 60, 50, 5, 5)       # 5 px gap < dilation
    _, text, _ = run(tmp_path, capsys, frame(), near)
    assert fields(text) == (50, 1, 15, 5, 50, 50)[:6] + (0,)
    far = patch(patch(frame(), 50, 50, 5, 5), 100, 50, 5, 5)       # 45 px gap
    _, text, _ = run(tmp_path, capsys, frame(), far)
    assert fields(text)[1] == 2


def test_diagonal_patches_connect(tmp_path, capsys):
    after = patch(patch(frame(), 50, 50, 3, 3), 53, 53, 3, 3)
    _, text, _ = run(tmp_path, capsys, frame(), after)
    assert fields(text)[1] == 1


def test_small_channel_delta_ignored_larger_counted(tmp_path, capsys):
    _, text, _ = run(tmp_path, capsys, frame(100), patch(frame(100), 10, 10, 20, 20, value=108))
    assert fields(text)[0] == 0
    _, text, _ = run(tmp_path, capsys, frame(100), patch(frame(100), 10, 10, 20, 20, value=109))
    assert fields(text)[0] == 400


def test_single_channel_change_counts(tmp_path, capsys):
    after = frame()
    after[10:20, 10:20, 2] = 140
    _, text, _ = run(tmp_path, capsys, frame(), after)
    assert fields(text)[:4] == (100, 1, 10, 10)


def test_threshold_exit_code(tmp_path, capsys):
    after = patch(frame(), 10, 10, 20, 20)  # 400 px
    rc, text, _ = run(tmp_path, capsys, frame(), after)
    assert rc == 1 and fields(text)[0] == 400
    rc, _, _ = run(tmp_path, capsys, frame(), after, "--threshold", "400")
    assert rc == 0


def test_largest_cluster_by_bbox_area(tmp_path, capsys):
    after = patch(patch(frame(), 10, 10, 4, 4), 200, 100, 30, 12)
    _, text, _ = run(tmp_path, capsys, frame(), after)
    assert fields(text)[1:6] == (2, 30, 12, 200, 100)


def test_large_noisy_mask_matches_expected_count(tmp_path, capsys):
    after = frame()
    for k in range(6):  # six well separated 8x8 blocks
        after[20:28, 20 + 60 * k:28 + 60 * k] = 220
    _, text, _ = run(tmp_path, capsys, frame(), after)
    assert fields(text)[:2] == (6 * 64, 6)


def test_montage_split_path(tmp_path, capsys):
    before = frame()
    after = patch(frame(), 120, 60, 10, 10)
    strip = np.concatenate([before, after, np.zeros_like(before)], axis=1)
    path = save(strip, tmp_path / "x_before_after_diff.png")
    out = str(tmp_path / "out.png")
    rc = mc.main(["--montage", path, "--out", out])
    assert rc == 0
    assert fields(capsys.readouterr().out.strip()) == (100, 1, 10, 10, 120, 60, 0)
    assert Image.open(out).size == (W // 4, H // 4)  # one frame, not the whole strip


def test_montage_width_not_divisible_by_three(tmp_path, capsys):
    path = save(np.zeros((10, 100, 3), dtype=np.uint8), tmp_path / "m.png")
    assert mc.main(["--montage", path, "--out", str(tmp_path / "o.png")]) == 2
    assert "divisible" in capsys.readouterr().err


def test_size_mismatch_exit_2(tmp_path, capsys):
    b = save(frame(), tmp_path / "b.png")
    a = save(np.zeros((H, W + 2, 3), dtype=np.uint8), tmp_path / "a.png")
    assert mc.main([b, a, "--out", str(tmp_path / "o.png")]) == 2
    assert "size mismatch" in capsys.readouterr().err


@pytest.mark.parametrize("box", ["1,2,3", "a,b,c,d", "0,0,0,5"])
def test_bad_mech_box_exit_2(tmp_path, capsys, box):
    b, a = save(frame(), tmp_path / "b.png"), save(frame(), tmp_path / "a.png")
    assert mc.main([b, a, "--mech-box", box, "--out", str(tmp_path / "o.png")]) == 2


def test_rgba_input_accepted(tmp_path, capsys):
    rgba = np.dstack([frame(), np.full((H, W), 255, dtype=np.uint8)])
    b = save(rgba, tmp_path / "b.png")
    a = save(np.dstack([patch(frame(), 5, 5, 6, 6), np.full((H, W), 255, dtype=np.uint8)]), tmp_path / "a.png")
    assert mc.main([b, a, "--out", str(tmp_path / "o.png")]) == 0
    assert fields(capsys.readouterr().out.strip())[:2] == (36, 1)
