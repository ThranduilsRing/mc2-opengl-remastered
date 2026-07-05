#!/usr/bin/env python3
"""pytest for overlay_extract.py: extract determinism, validate error cases,
install round-trip, and (skip-if-missing) extract against real stock .paks."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOL = HERE / "overlay_extract.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")


def run(*args) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True,
    )


def _extract(pak_stem: str, out: Path, supersample: int = 1) -> subprocess.CompletedProcess:
    return run("extract", "--pak", str(CARVER5_MISSIONS / f"{pak_stem}.pak"),
               "--out", str(out), "--supersample", str(supersample))


# --- extract: determinism + basic shape --------------------------------------
@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_deterministic(tmp_path):
    out1 = tmp_path / "a.png"
    out2 = tmp_path / "b.png"
    r1 = _extract("mc2_24", out1)
    r2 = _extract("mc2_24", out2)
    assert r1.returncode == 0, r1.stdout + r1.stderr
    assert r2.returncode == 0, r2.stdout + r2.stderr
    a1 = np.array(Image.open(out1))
    a2 = np.array(Image.open(out2))
    assert np.array_equal(a1, a2)
    # companion bounds files must match too
    b1 = out1.with_suffix("").with_name(out1.with_suffix("").name + ".bounds.txt").read_text()
    b2 = out2.with_suffix("").with_name(out2.with_suffix("").name + ".bounds.txt").read_text()
    assert b1 == b2


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_mc2_24_has_concrete_coverage(tmp_path):
    """mc2_24 is the cement-heavy tier1 mission (memory: 8282 draw stat) --
    the extractor must find a non-trivial amount of concrete."""
    out = tmp_path / "mc2_24_overlay.png"
    r = _extract("mc2_24", out)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "concrete_cells=" in r.stdout
    cells = int(r.stdout.split("concrete_cells=")[1].split()[0])
    assert cells > 0, "expected mc2_24 to have concrete cells (it's the cement-heavy tier1 mission)"

    arr = np.array(Image.open(out).convert("RGBA"))
    coverage = int((arr[..., 3] >= 128).sum())
    assert coverage == cells  # supersample=1 -> 1:1 texel:cell


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_supersample_scales_resolution(tmp_path):
    out1 = tmp_path / "ss1.png"
    out2 = tmp_path / "ss2.png"
    r1 = _extract("mc2_24", out1, supersample=1)
    r2 = _extract("mc2_24", out2, supersample=2)
    assert r1.returncode == 0 and r2.returncode == 0
    w1, h1 = Image.open(out1).size
    w2, h2 = Image.open(out2).size
    assert w2 == 2 * w1 and h2 == 2 * h1


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_output_validates(tmp_path):
    out = tmp_path / "mc2_24_ov.png"
    _extract("mc2_24", out)
    r = run("validate", "--png", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "PASS" in r.stdout


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_bridge_gate_exclusion_does_not_swallow_non_bridge_concrete():
    """Regression for the latent bridge/gate exclusion bug (flagged by
    WHOLESALE-VECTORIZE-1, root-fixed here): `read_bridge_gate_overlay_cells`
    must actually discriminate bridge/gate cells (runtime-mutable, water-
    adjacent) from ordinary overlay-tagged concrete (road/runway paint on dry
    land). The old body returned the SAME mask as "any overlay-tagged cell"
    (overlay_hi != 0 and != 0xFFFF, no water test at all), so it wrongly
    excluded every overlay-tagged concrete cell from the sidecar bake --
    including cells nowhere near water. On mc2_24 this drops 449 of 1170
    concrete cells (38%) from the baked overlay-v2 sidecar even though NONE
    of them are within 2 cells of water (i.e. none are bridge/gate-like)."""
    from mission_terrain_analyzer import read_packets, locate_mapdata, extract_layers, read_water_elevation
    from overlay_extract import classify_concrete, read_bridge_gate_overlay_cells

    pak = CARVER5_MISSIONS / "mc2_24.pak"
    packets = read_packets(pak)
    pkt_idx, side, blocks = locate_mapdata(packets)
    water_elev = read_water_elevation(pak.with_suffix(".fit"))
    layers = extract_layers(side, blocks, water_elev)
    water = layers["water"]
    concrete = classify_concrete(layers["terrtype"])

    bridge_gate = read_bridge_gate_overlay_cells(side, blocks, water_elev)

    # A real bridge/gate cell must be at or adjacent to water. Any excluded
    # concrete cell that is nowhere near water is a false positive: ordinary
    # road/runway concrete wrongly dropped from the bake.
    dilated = water.copy()
    for _ in range(2):
        nxt = dilated.copy()
        nxt[1:, :] |= dilated[:-1, :]
        nxt[:-1, :] |= dilated[1:, :]
        nxt[:, 1:] |= dilated[:, :-1]
        nxt[:, :-1] |= dilated[:, 1:]
        dilated = nxt

    excluded_concrete = concrete & bridge_gate
    false_positive_exclusions = int((excluded_concrete & ~dilated).sum())
    assert false_positive_exclusions == 0, (
        f"{false_positive_exclusions} concrete cells excluded as 'bridge/gate' "
        "despite being nowhere near water -- exclusion mask is too broad "
        "(matches ANY overlay-tagged cell, not just bridge/gate cells)")

    # And it must still classify at least mc2_24's known concrete coverage
    # once the false-positive exclusion is fixed (no regression to 0 baked
    # concrete on the mission this slice ships for).
    kept = int((concrete & ~bridge_gate).sum())
    assert kept == int(concrete.sum()), (
        "expected NO concrete cells excluded on mc2_24 (0 real bridge/gate "
        "cells there per recon), but the fix dropped some")


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_pak_untouched_byte_identical(tmp_path):
    """Read-only w.r.t. .pak -- extraction must never mutate the source pak."""
    pak = CARVER5_MISSIONS / "mc2_01.pak"
    before = pak.read_bytes()
    out = tmp_path / "mc2_01_ov.png"
    _extract("mc2_01", out)
    after = pak.read_bytes()
    assert before == after


# --- validate -----------------------------------------------------------------
def test_validate_missing_file_fails(tmp_path):
    r = run("validate", "--png", str(tmp_path / "nope.png"))
    assert r.returncode == 1


def test_validate_wrong_mode_fails(tmp_path):
    png = tmp_path / "rgb.png"
    Image.new("RGB", (60, 60), (10, 20, 30)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_no_bounds_warns_but_passes(tmp_path):
    png = tmp_path / "solo.png"
    Image.new("RGBA", (32, 32), (0, 0, 0, 0)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 0
    assert "WARN" in r.stdout


def test_validate_bad_bounds_fails(tmp_path):
    png = tmp_path / "solo.png"
    Image.new("RGBA", (32, 32), (0, 0, 0, 0)).save(png)
    bounds = tmp_path / "solo.bounds.txt"
    bounds.write_text("1.0 2.0 -5.0 3.0\n")  # negative sizeX
    r = run("validate", "--png", str(png), "--bounds", str(bounds))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_good_bounds_passes(tmp_path):
    png = tmp_path / "solo.png"
    Image.new("RGBA", (32, 32), (0, 0, 0, 0)).save(png)
    bounds = tmp_path / "solo.bounds.txt"
    bounds.write_text("-1000.0 1000.0 2000.0 2000.0\n")
    r = run("validate", "--png", str(png), "--bounds", str(bounds))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "PASS" in r.stdout


# --- sheet ----------------------------------------------------------------
def test_sheet_runs(tmp_path):
    png = tmp_path / "p.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    rgba[..., 3] = 255
    Image.fromarray(rgba, "RGBA").save(png)
    out = tmp_path / "sheet.png"
    r = run("sheet", "--png", str(png), "--out", str(out))
    assert r.returncode == 0, r.stderr
    assert out.is_file()


# --- install ----------------------------------------------------------------
def test_install_copies_to_beauty_dir(tmp_path):
    png = tmp_path / "src.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    Image.fromarray(rgba, "RGBA").save(png)
    bounds = tmp_path / "src.bounds.txt"
    bounds.write_text("-100.0 100.0 200.0 200.0\n")
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_24")
    assert r.returncode == 0, r.stdout + r.stderr
    dest = deploy / "data" / "missions" / "mc2_24.beauty" / "overlay_v2.png"
    assert dest.is_file()
    dest_bounds = deploy / "data" / "missions" / "mc2_24.beauty" / "overlay_v2.bounds.txt"
    assert dest_bounds.is_file()
    assert dest_bounds.read_text() == bounds.read_text()


def test_install_without_bounds_warns(tmp_path):
    png = tmp_path / "src_nobounds.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    Image.fromarray(rgba, "RGBA").save(png)
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_24")
    assert r.returncode == 0
    assert "WARN" in r.stdout
