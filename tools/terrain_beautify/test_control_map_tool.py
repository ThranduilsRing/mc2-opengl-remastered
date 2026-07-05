#!/usr/bin/env python3
"""pytest for control_map_tool.py: pattern determinism, validate error cases,
and (skip-if-missing) generate against a real Carver5 stock .pak."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOL = HERE / "control_map_tool.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")

sys.path.insert(0, str(HERE))
from control_map_tool import (  # noqa: E402
    compute_slope_deg, classify_weights, area_average_downsample, weights_to_rgba,
)


def run(*args) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True,
    )


# --- pattern determinism -----------------------------------------------------
def test_pattern_quadrant_deterministic(tmp_path):
    out1 = tmp_path / "a.png"
    out2 = tmp_path / "b.png"
    r1 = run("pattern", "--side", "60", "--out", str(out1), "--kind", "quadrant")
    r2 = run("pattern", "--side", "60", "--out", str(out2), "--kind", "quadrant")
    assert r1.returncode == 0, r1.stderr
    assert r2.returncode == 0, r2.stderr
    a1 = np.array(Image.open(out1))
    a2 = np.array(Image.open(out2))
    assert np.array_equal(a1, a2)


def test_pattern_quadrant_corners(tmp_path):
    out = tmp_path / "q.png"
    run("pattern", "--side", "60", "--out", str(out), "--kind", "quadrant")
    arr = np.array(Image.open(out).convert("RGBA"))
    side = arr.shape[0]
    # Row 0 == top per documented row-order convention.
    assert tuple(arr[0, 0]) == (255, 0, 0, 0)               # top-left: rock
    assert tuple(arr[0, side - 1]) == (0, 255, 0, 0)        # top-right: grass
    assert tuple(arr[side - 1, 0]) == (0, 0, 255, 0)        # bottom-left: dirt
    assert tuple(arr[side - 1, side - 1]) == (0, 0, 0, 255)  # bottom-right: concrete


def test_pattern_gradient_shape_and_range(tmp_path):
    out = tmp_path / "g.png"
    r = run("pattern", "--side", "80", "--out", str(out), "--kind", "gradient")
    assert r.returncode == 0, r.stderr
    arr = np.array(Image.open(out).convert("RGBA"))
    assert arr.shape == (80, 80, 4)
    # R decreases left->right, G increases left->right, each row identical.
    row = arr[0]
    assert row[0, 0] > row[-1, 0]
    assert row[0, 1] < row[-1, 1]
    assert np.array_equal(arr[0], arr[-1])  # same gradient every row


def test_pattern_checker_binary_channels(tmp_path):
    out = tmp_path / "c.png"
    r = run("pattern", "--side", "64", "--out", str(out), "--kind", "checker")
    assert r.returncode == 0, r.stderr
    arr = np.array(Image.open(out).convert("RGBA"))
    # Every texel is pure rock XOR pure grass, dirt/concrete unused.
    assert np.all(arr[..., 2] == 0)
    assert np.all(arr[..., 3] == 0)
    is_rock = arr[..., 0] == 255
    is_grass = arr[..., 1] == 255
    assert np.all(is_rock ^ is_grass)


def test_pattern_bad_kind_fails(tmp_path):
    out = tmp_path / "bad.png"
    r = run("pattern", "--side", "60", "--out", str(out), "--kind", "notarealkind")
    assert r.returncode != 0


def test_pattern_side_too_small_fails(tmp_path):
    out = tmp_path / "tiny.png"
    r = run("pattern", "--side", "1", "--out", str(out), "--kind", "quadrant")
    assert r.returncode != 0


# --- validate -----------------------------------------------------------------
def test_validate_correct_size_passes(tmp_path):
    png = tmp_path / "ok.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    r = run("validate", "--png", str(png), "--side", "60")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "PASS" in r.stdout


def test_validate_wrong_size_fails(tmp_path):
    png = tmp_path / "wrong.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    r = run("validate", "--png", str(png), "--side", "80")
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_non_square_fails(tmp_path):
    png = tmp_path / "nonsquare.png"
    Image.new("RGBA", (60, 40), (0, 0, 0, 0)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_wrong_mode_fails(tmp_path):
    png = tmp_path / "rgb.png"
    Image.new("RGB", (60, 60), (10, 20, 30)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_missing_file_fails(tmp_path):
    r = run("validate", "--png", str(tmp_path / "nope.png"))
    assert r.returncode == 1


# --- sheet ----------------------------------------------------------------
def test_sheet_runs_on_pattern(tmp_path):
    png = tmp_path / "p.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    out = tmp_path / "sheet.png"
    r = run("sheet", "--png", str(png), "--out", str(out))
    assert r.returncode == 0, r.stderr
    assert out.is_file()


# --- install ----------------------------------------------------------------
def test_install_copies_to_beauty_dir(tmp_path):
    png = tmp_path / "src.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_01")
    assert r.returncode == 0, r.stderr
    dest = deploy / "data" / "missions" / "mc2_01.beauty" / "control_map.png"
    assert dest.is_file()


# --- generate (skip if Carver5 stock data absent) ----------------------------
@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_generate_runs_on_mc2_01(tmp_path):
    out = tmp_path / "mc2_01_control_map.png"
    r = run("generate", "--pak", str(CARVER5_MISSIONS / "mc2_01.pak"), "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert out.is_file()
    img = Image.open(out)
    assert img.mode == "RGBA"
    w, h = img.size
    assert w == h
    assert w in (60, 80, 100, 120)
    # every texel should classify to exactly one material (one-hot 0/255 channels)
    arr = np.array(img)
    total = arr.sum(axis=-1)
    assert np.all((total == 255) | (total == 0))


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_generate_output_validates(tmp_path):
    out = tmp_path / "mc2_01_cm.png"
    run("generate", "--pak", str(CARVER5_MISSIONS / "mc2_01.pak"), "--out", str(out))
    r = run("validate", "--png", str(out), "--pak", str(CARVER5_MISSIONS / "mc2_01.pak"))
    assert r.returncode == 0, r.stdout + r.stderr


# --- generate-from-height: slope math sanity --------------------------------
def test_slope_math_flat_plane_is_zero():
    res = 64
    h = np.full((res, res), 0.5)
    slope = compute_slope_deg(h, size_wu=8192.0)
    assert np.allclose(slope, 0.0)


def test_slope_math_synthetic_ramp():
    """A linear ramp of known rise-over-run must produce the exact analytic
    slope angle (wu-correct: step_wu = size_wu / (res-1))."""
    res = 100
    size_wu = 1280.0
    height_wu = np.tile(np.linspace(0.0, 1.0, res), (res, 1))  # 1 wu of rise, left->right
    slope = compute_slope_deg(height_wu, size_wu=size_wu)
    step_wu = size_wu / (res - 1)
    expected_deg = np.degrees(np.arctan((1.0 / (res - 1)) / step_wu))
    # interior columns only (np.gradient uses one-sided differences at edges,
    # which are still correct here since the ramp is perfectly linear).
    assert np.allclose(slope[10:90, 10:90], expected_deg, atol=1e-9)


def test_slope_math_steeper_ramp_yields_larger_angle():
    res = 100
    gentle = np.tile(np.linspace(0.0, 0.1, res), (res, 1))
    steep = np.tile(np.linspace(0.0, 1.0, res), (res, 1))
    slope_gentle = compute_slope_deg(gentle, size_wu=1280.0)
    slope_steep = compute_slope_deg(steep, size_wu=1280.0)
    assert slope_steep[50, 50] > slope_gentle[50, 50]


def test_slope_math_size_wu_scales_inversely():
    """Doubling the world-unit extent (same texel grid) halves the step slope
    for a fixed height delta -> smaller angle."""
    res = 64
    h = np.tile(np.linspace(0.0, 1.0, res), (res, 1))
    slope_small_extent = compute_slope_deg(h, size_wu=1000.0)
    slope_large_extent = compute_slope_deg(h, size_wu=4000.0)
    assert slope_small_extent[30, 30] > slope_large_extent[30, 30]


# --- generate-from-height: classifier determinism ---------------------------
def test_classify_weights_deterministic():
    rs = np.random.RandomState(42)
    h = rs.uniform(0.0, 1.0, (48, 48))
    w1 = classify_weights(h, size_wu=6144.0, max_elev_wu=1200.0)
    w2 = classify_weights(h, size_wu=6144.0, max_elev_wu=1200.0)
    assert np.array_equal(w1["rock"], w2["rock"])
    assert np.array_equal(w1["grass"], w2["grass"])
    assert np.array_equal(w1["dirt"], w2["dirt"])


def test_classify_weights_sum_to_one():
    rs = np.random.RandomState(7)
    h = rs.uniform(0.0, 1.0, (32, 32))
    w = classify_weights(h, size_wu=4096.0, max_elev_wu=1200.0)
    total = w["rock"] + w["grass"] + w["dirt"]
    assert np.allclose(total, 1.0, atol=1e-9)


def test_classify_weights_flat_low_is_grass_dominant():
    """A perfectly flat, low-altitude plateau should classify overwhelmingly
    as grass (low slope AND low altitude -> grass rule)."""
    h = np.full((32, 32), 0.05)  # flat, well below low_frac=0.35 default
    w = classify_weights(h, size_wu=4096.0, max_elev_wu=1200.0)
    assert w["grass"].mean() > 0.9


def test_classify_weights_steep_high_is_rock_dominant():
    """A steep, high-altitude ramp (mountains) should classify overwhelmingly
    as rock (steep OR high -> rock rule) -- the "mountains read as mountains"
    acceptance criterion."""
    res = 48
    ramp = np.tile(np.linspace(0.7, 1.0, res), (res, 1))  # steep + high (>0.65 high_frac)
    w = classify_weights(ramp, size_wu=1536.0, max_elev_wu=1200.0)  # small extent -> steep
    assert w["rock"].mean() > 0.8


def test_classify_weights_rock_mask_boosts_rock():
    h = np.full((32, 32), 0.05)  # would otherwise be grass-dominant
    mask = np.ones((32, 32))
    w_plain = classify_weights(h, size_wu=4096.0, max_elev_wu=1200.0)
    w_masked = classify_weights(h, size_wu=4096.0, max_elev_wu=1200.0, rock_mask=mask)
    assert w_masked["rock"].mean() > w_plain["rock"].mean()


def test_classify_weights_sediment_mask_boosts_dirt():
    h = np.full((32, 32), 0.05)
    mask = np.ones((32, 32))
    w_plain = classify_weights(h, size_wu=4096.0, max_elev_wu=1200.0)
    w_masked = classify_weights(h, size_wu=4096.0, max_elev_wu=1200.0, sediment_mask=mask)
    assert w_masked["dirt"].mean() > w_plain["dirt"].mean()


# --- generate-from-height: downsample + packing ------------------------------
def test_area_average_downsample_preserves_mean():
    rs = np.random.RandomState(3)
    a = rs.uniform(0.0, 1.0, (256, 256))
    ds = area_average_downsample(a, 64)
    assert ds.shape == (64, 64)
    assert abs(float(ds.mean()) - float(a.mean())) < 0.02


def test_area_average_downsample_noop_when_same_size():
    a = np.arange(16, dtype=np.float64).reshape(4, 4)
    ds = area_average_downsample(a, 4)
    assert np.array_equal(ds, a)


def test_weights_to_rgba_packs_and_sums_255():
    rock = np.full((8, 8), 0.5)
    grass = np.full((8, 8), 0.3)
    dirt = np.full((8, 8), 0.2)
    rgba = weights_to_rgba(rock, grass, dirt)
    assert rgba.shape == (8, 8, 4)
    assert np.all(rgba[..., 3] == 0)  # A=concrete always 0 from this classifier
    total = rgba[..., :3].astype(np.int32).sum(axis=-1)
    assert np.all(np.abs(total.astype(np.int64) - 255) <= 1)  # rounding tolerance


# --- generate-from-height: CLI end-to-end (synthetic, no external assets) ---
def test_cli_generate_from_height_synthetic_r32(tmp_path):
    res, side = 64, 20
    rs = np.random.RandomState(1)
    h = rs.uniform(0.0, 1.0, (res, res)).astype("<f4")
    r32 = tmp_path / "synthetic.r32"
    h.tofile(r32)
    out = tmp_path / "cm.png"
    r = run("generate-from-height", "--r32", str(r32), "--res", str(res),
            "--side", str(side), "--size", "8192", "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert out.is_file()
    img = Image.open(out)
    assert img.mode == "RGBA"
    assert img.size == (side, side)


def test_cli_generate_from_height_deterministic(tmp_path):
    res, side = 32, 20
    rs = np.random.RandomState(2)
    h = rs.uniform(0.0, 1.0, (res, res)).astype("<f4")
    r32 = tmp_path / "synthetic.r32"
    h.tofile(r32)
    out1, out2 = tmp_path / "a.png", tmp_path / "b.png"
    r1 = run("generate-from-height", "--r32", str(r32), "--res", str(res),
              "--side", str(side), "--size", "4096", "--out", str(out1))
    r2 = run("generate-from-height", "--r32", str(r32), "--res", str(res),
              "--side", str(side), "--size", "4096", "--out", str(out2))
    assert r1.returncode == 0 and r2.returncode == 0
    assert np.array_equal(np.array(Image.open(out1)), np.array(Image.open(out2)))


def test_cli_generate_from_height_requires_source(tmp_path):
    out = tmp_path / "cm.png"
    r = run("generate-from-height", "--side", "20", "--out", str(out))
    assert r.returncode != 0


def test_cli_generate_from_height_output_validates(tmp_path):
    res, side = 40, 20
    rs = np.random.RandomState(5)
    h = rs.uniform(0.0, 1.0, (res, res)).astype("<f4")
    r32 = tmp_path / "synthetic.r32"
    h.tofile(r32)
    out = tmp_path / "cm.png"
    run("generate-from-height", "--r32", str(r32), "--res", str(res),
        "--side", str(side), "--size", "2560", "--out", str(out))
    r = run("validate", "--png", str(out), "--side", str(side))
    assert r.returncode == 0, r.stdout + r.stderr
