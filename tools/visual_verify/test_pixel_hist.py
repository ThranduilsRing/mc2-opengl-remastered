"""What: tests for pixel_hist.py - identical/noise/missing-block/brightness-shift synthetic pairs get the right histogram verdict and region bbox, GPU layers resolve, one archived real pair passes.
Why: MAD cannot tell a missing building from a harmless global shift; the pixel-count buckets + largest-region bbox can."""
import os, sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pixel_hist as ph

TH = ph.resolve_thresholds(ph.load_thresholds())
W, H = 640, 360


def scene():
    rng = np.random.default_rng(7)
    base = rng.integers(60, 160, (H, W, 3), dtype=np.uint8)
    return base


def test_identical_passes_with_no_region():
    a = scene()
    r = ph.evaluate(a, a.copy(), TH)
    assert r["verdict"] == "PASS" and r["region"] is None and r["max_delta"] == 0


def test_sparse_noise_passes():
    a = scene()
    b = a.copy()
    rng = np.random.default_rng(1)
    ys, xs = rng.integers(0, H, 300), rng.integers(0, W, 300)  # 0.13% of pixels, +-3 levels
    b[ys, xs] = np.clip(b[ys, xs].astype(int) + rng.integers(-3, 4, (300, 3)), 0, 255).astype(np.uint8)
    r = ph.evaluate(a, b, TH)
    assert r["verdict"] == "PASS", r["reasons"]


def test_missing_block_fails_and_is_located():
    a = scene()
    b = a.copy()
    b[100:140, 200:240] = 0  # a 40x40 building vanishes
    r = ph.evaluate(a, b, TH)
    assert r["verdict"] == "FAIL"
    assert r["region"]["bbox"] == [200, 100, 240, 140] and r["region"]["pixels"] == 1600
    assert any("over delta 64" in x for x in r["reasons"])
    mad = float(np.abs(a.astype(int) - b).mean())
    assert mad < 1.0  # why MAD alone is no signal: a missing object barely moves it (see the brightness-shift test)


def test_global_brightness_shift_fails_only_low_buckets_without_region_blob_bias():
    a = scene()
    b = np.clip(a.astype(int) + 12, 0, 255).astype(np.uint8)
    r = ph.evaluate(a, b, TH)
    assert r["verdict"] == "FAIL" and r["region"] is None  # nothing over delta 24
    assert all("over delta 8" in x for x in r["reasons"])
    assert float(np.abs(a.astype(int) - b).mean()) > 11  # MAD would call this far worse than the missing block


def test_largest_region_picks_the_bigger_blob():
    d = np.zeros((200, 200), np.uint8)
    d[10:20, 10:20] = 200
    d[100:150, 100:160] = 200
    reg = ph.largest_region(d)
    assert reg["bbox"] == [100, 100, 160, 150] and reg["pixels"] == 3000


def test_gpu_and_bookmark_layers():
    cfg = {"default": {"a": 1, "b": 1}, "gpu": {"7900": {"b": 2}},
           "bookmarks": {"bm": {"a": 3, "gpu": {"2070": {"a": 4}}}}}
    assert ph.resolve_thresholds(cfg) == {"a": 1, "b": 1}
    assert ph.resolve_thresholds(cfg, "bm", ["AMD Radeon RX 7900 XTX"]) == {"a": 3, "b": 2}
    assert ph.resolve_thresholds(cfg, "bm", "NVIDIA GeForce RTX 2070 SUPER") == {"a": 4, "b": 1}


REAL = Path("E:/mc2-archive-2026-10/proofs/claude-vk-cpu-frontier-1")


@pytest.mark.skipif(not (REAL / "capture-before-day").is_dir(), reason="archived proofs not present")
def test_real_archived_pair_passes_and_missing_block_on_it_fails():
    from PIL import Image
    name = "mc2_01_near_land_low.png"
    a = np.asarray(Image.open(REAL / "capture-before-day" / name).convert("RGB"))
    b = np.asarray(Image.open(REAL / "capture-after-day" / name).convert("RGB"))
    assert ph.evaluate(a, b, TH)["verdict"] == "PASS"
    c = a.copy()
    c[1000:1040, 2000:2040] //= 4  # darken a 40x40 patch of the real frame
    r = ph.evaluate(a, c, TH)
    assert r["verdict"] == "FAIL" and r["region"]["bbox"] == [2000, 1000, 2040, 1040]
