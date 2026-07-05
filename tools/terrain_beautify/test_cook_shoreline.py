#!/usr/bin/env python3
"""pytest for cook_shoreline.py: signed-EDT math on a synthetic pond fixture,
determinism, band widths, hi-res-vs-coarse source selection, and (skip-if-
missing) cook against a real stock mission."""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOL = HERE / "cook_shoreline.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")

sys.path.insert(0, str(HERE))
import cook_shoreline as cs  # noqa: E402

PCV_SIZE = 32
OFF_ELEV = 12
OFF_TEXDATA = 16
OFF_TERRTYPE = 24
PACKET_MAGIC = 0xFEEDFACE


def run(*args) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True,
    )


# --- unit tests: signed-distance math (no fixtures needed) -------------------
def test_signed_distance_sign_and_zero_crossing():
    """A synthetic 20x20 grid, water in the left half (cols<10), land in the
    right half. Signed distance must be negative in water, positive on land,
    and the zero crossing must sit exactly at the water/land boundary."""
    side = 20
    water = np.zeros((side, side), dtype=bool)
    water[:, :10] = True
    d = cs.compute_signed_distance_wu(water, cell_wu=128.0)
    assert (d[water] < 0).all()
    assert (d[~water] > 0).all()
    # column 9 (last water col) is adjacent to column 10 (first land col) ->
    # both within one cell of the boundary.
    assert abs(d[5, 9]) <= 128.0
    assert abs(d[5, 10]) <= 128.0


def test_signed_distance_scales_with_cell_size():
    """Distances scale linearly with the cell_wu (grid spacing) parameter --
    this is what lets the hi-res grid (finer cell_wu) carry sub-tile curvature
    instead of being locked to the 128wu vertex grid."""
    side = 20
    water = np.zeros((side, side), dtype=bool)
    water[:, :5] = True
    d_coarse = cs.compute_signed_distance_wu(water, cell_wu=128.0)
    d_fine = cs.compute_signed_distance_wu(water, cell_wu=32.0)
    # same topology, 4x finer cell spacing -> magnitudes scale by 1/4.
    np.testing.assert_allclose(d_fine, d_coarse / 4.0, atol=1e-6)


def test_build_mask_rgba_channels_synthetic_pond():
    """Synthetic circular pond: verify R crosses 128 at the edge, G (wet) is
    nonzero only on the land side within damp_w, B (foam) straddles the edge,
    A is always 255."""
    side = 64
    yy, xx = np.mgrid[0:side, 0:side]
    cy, cx = side / 2, side / 2
    radius = 15.0
    water = ((yy - cy) ** 2 + (xx - cx) ** 2) < radius ** 2
    # cell_wu=1.0 (fine spacing): isolates the RGBA-band math from grid
    # resolution -- a 128wu-spaced coarse grid cannot resolve a 24wu foam band
    # at all (min nonzero EDT step == cell_wu), which is exactly why the recon
    # requires hi-res sourcing; see test_cook_falls_back_to_coarse_foam_is_empty.
    d = cs.compute_signed_distance_wu(water, cell_wu=1.0)
    rgba = cs.build_mask_rgba(d, damp_w=64.0, foam_w=24.0)

    assert rgba.shape == (side, side, 4)
    assert rgba.dtype == np.uint8
    assert (rgba[..., 3] == 255).all()  # A always valid for a full cook

    # G (wet) must be 0 strictly inside the water (d<0 clamped to 0 land_d).
    assert (rgba[water][:, 1] == 0).all()
    # G (wet) must be > 0 immediately outside the water edge (within damp_w).
    land_near_edge = (~water) & (d < 32.0)
    assert land_near_edge.any()
    assert (rgba[land_near_edge][:, 1] > 0).any()

    # B (foam) must be nonzero near the waterline on both sides.
    near_edge = np.abs(d) < 10.0
    assert near_edge.any()
    assert (rgba[near_edge][:, 2] > 0).any()


def test_build_mask_rgba_band_width_floor():
    """Band widths below MIN_BAND_WU are NOT requested directly by
    build_mask_rgba (the cook CLI clamps first) -- but build_mask_rgba itself
    must not divide-by-zero or produce NaN/inf even if called with a
    pathologically small width, matching the recon's floor discipline."""
    side = 10
    water = np.zeros((side, side), dtype=bool)
    water[:, :5] = True
    d = cs.compute_signed_distance_wu(water, cell_wu=128.0)
    rgba = cs.build_mask_rgba(d, damp_w=1e-9, foam_w=1e-9)
    assert np.isfinite(rgba).all()
    assert rgba.dtype == np.uint8


def test_wider_damp_band_widens_wet_coverage():
    side = 40
    water = np.zeros((side, side), dtype=bool)
    water[:, :10] = True
    d = cs.compute_signed_distance_wu(water, cell_wu=128.0)
    narrow = cs.build_mask_rgba(d, damp_w=32.0, foam_w=16.0)
    wide = cs.build_mask_rgba(d, damp_w=256.0, foam_w=16.0)
    assert int((wide[..., 1] > 0).sum()) >= int((narrow[..., 1] > 0).sum())


# --- fixture helpers: synthetic .pak + .fit --------------------------------
def _write_synthetic_pak(pak_path: Path, side: int, water_elev: float, island_radius: float):
    """Minimal PacketFile with a single RAW MapData packet: an island (high
    elevation) surrounded by water (elevation below water_elev), so
    read_water_elevation + extract_layers can round-trip it exactly like a
    real stock mission."""
    yy, xx = np.mgrid[0:side, 0:side].astype(np.float64)
    cy, cx = side / 2.0, side / 2.0
    dist = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2)
    elev = (island_radius - dist) * 4.0  # positive inside radius, negative outside
    elev = elev.astype(np.float32)

    n = side * side
    blocks = bytearray(n * PCV_SIZE)
    for i in range(n):
        base = i * PCV_SIZE
        struct.pack_into("<f", blocks, base + OFF_ELEV, float(elev.flat[i]))
        struct.pack_into("<I", blocks, base + OFF_TEXDATA, 0)
        struct.pack_into("<I", blocks, base + OFF_TERRTYPE, 0)
    payload = bytes(blocks)

    # PacketFile: header (magic + first_off) + 1 entry (RAW type, offset=8+4) + payload.
    entry_off = 8 + 4  # 1 entry table (4 bytes) after the 8-byte header
    entry = (0x0 << 29) | entry_off  # ST_RAW
    header = struct.pack("<I", PACKET_MAGIC) + struct.pack("<I", entry_off)
    table = struct.pack("<I", entry)
    pak_path.write_bytes(header + table + payload)
    return elev


def _write_fit(fit_path: Path, water_elev: float):
    fit_path.write_text(f"[Water]\nf Elevation = {water_elev}\n")


@pytest.fixture()
def synthetic_island(tmp_path):
    side = 60  # VALID_GRID_SIDES member
    water_elev = 0.0
    pak = tmp_path / "synth_island.pak"
    fit = tmp_path / "synth_island.fit"
    elev = _write_synthetic_pak(pak, side, water_elev, island_radius=12.0)
    _write_fit(fit, water_elev)
    return {"pak": pak, "fit": fit, "side": side, "elev": elev, "water_elev": water_elev}


# --- cook: end-to-end determinism -------------------------------------------
def test_cook_deterministic(synthetic_island, tmp_path):
    out1 = tmp_path / "a.png"
    out2 = tmp_path / "b.png"
    r1 = run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out1))
    r2 = run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out2))
    assert r1.returncode == 0, r1.stdout + r1.stderr
    assert r2.returncode == 0, r2.stdout + r2.stderr
    a1 = np.array(Image.open(out1))
    a2 = np.array(Image.open(out2))
    assert np.array_equal(a1, a2)
    b1 = out1.with_suffix("").with_name(out1.with_suffix("").name + ".bounds.txt").read_text()
    b2 = out2.with_suffix("").with_name(out2.with_suffix("").name + ".bounds.txt").read_text()
    assert b1 == b2


def test_cook_synthetic_island_produces_nonempty_bands(synthetic_island, tmp_path):
    """--supersample refines the coarse 128wu grid before the EDT (same effect
    as a hi-res bake for this test's purposes) so a 24wu foam band has
    sub-cell resolution to land on -- see test_cook_falls_back_to_coarse_foam_
    is_empty for the un-supersampled (landmine) case."""
    out = tmp_path / "island.png"
    r = run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out),
            "--damp-width", "64", "--foam-width", "24", "--supersample", "8")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "water_cells=" in r.stdout
    arr = np.array(Image.open(out).convert("RGBA"))
    assert (arr[..., 1] > 0).any(), "expected nonzero wet band"
    assert (arr[..., 2] > 0).any(), "expected nonzero foam band"
    assert (arr[..., 3] == 255).all()


def test_cook_falls_back_to_coarse_when_no_hires_bake(synthetic_island, tmp_path):
    """No .beauty/visual_height_4x.r32 sits next to the synthetic pak ->
    the cook must fall back to the coarse pak heightfield and say so."""
    out = tmp_path / "coarse.png"
    r = run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "fallback" in r.stdout


def test_cook_coarse_fallback_narrow_foam_band_is_empty(synthetic_island, tmp_path):
    """Documents the exact landmine the recon's hi-res-sourcing amendment
    exists to avoid: at the coarse 128wu vertex grid, the EDT's minimum
    nonzero step IS 128wu, so a foam band narrower than that (the default
    24wu) can never appear -- only a hi-res source (or --supersample) can
    resolve sub-tile foam width. This is expected/documented behavior, not a
    bug: the cook still succeeds and produces a valid (if foam-band-empty)
    mask, matching the recon's "fallback to pak height only when no hi-res
    source exists" ruling."""
    out = tmp_path / "coarse_narrow_foam.png"
    r = run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out),
            "--foam-width", "24")
    assert r.returncode == 0, r.stdout + r.stderr
    arr = np.array(Image.open(out).convert("RGBA"))
    assert not (arr[..., 2] > 0).any(), (
        "expected the coarse 128wu grid to be too sparse for a 24wu foam band "
        "-- if this fails, either the grid resolution changed or foam math changed")


def test_cook_uses_hires_source_when_present(synthetic_island, tmp_path):
    """Write a 4x-finer visual_height_4x.r32 bake next to the pak (same island
    shape, upsampled) and confirm the cook picks it up and reports the hi-res
    source + a finer mask resolution than the coarse grid_side."""
    side = synthetic_island["side"]
    factor = 4
    V = (side - 1) * factor + 1
    yy, xx = np.mgrid[0:V, 0:V].astype(np.float64)
    cy, cx = V / 2.0, V / 2.0
    # keep the same world-space island radius (12 coarse cells * 128wu/vertex
    # -> radius in fine-grid cells scales by `factor`).
    dist = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2)
    fine_elev = ((12.0 * factor) - dist) * 4.0
    fine_elev = fine_elev.astype(np.float32)

    beauty_dir = synthetic_island["pak"].parent / f"{synthetic_island['pak'].stem}.beauty"
    beauty_dir.mkdir(parents=True, exist_ok=True)
    vh_path = beauty_dir / "visual_height_4x.r32"
    fine_elev.tofile(vh_path)

    out = tmp_path / "hires.png"
    r = run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "hi-res visual_height" in r.stdout
    assert f"mask={V}x{V}" in r.stdout


def test_cook_explicit_visual_height_override(synthetic_island, tmp_path):
    side = synthetic_island["side"]
    factor = 2
    V = (side - 1) * factor + 1
    fine_elev = np.zeros((V, V), dtype=np.float32)
    vh_path = tmp_path / "custom_vh.r32"
    fine_elev.tofile(vh_path)

    out = tmp_path / "override.png"
    r = run("cook", "--pak", str(synthetic_island["pak"]), "--visual-height", str(vh_path),
            "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "hi-res visual_height" in r.stdout
    assert f"mask={V}x{V}" in r.stdout


def test_cook_band_widths_affect_coverage(synthetic_island, tmp_path):
    out_narrow = tmp_path / "narrow.png"
    out_wide = tmp_path / "wide.png"
    run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out_narrow),
        "--damp-width", "16")
    run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out_wide),
        "--damp-width", "512")
    narrow = np.array(Image.open(out_narrow).convert("RGBA"))
    wide = np.array(Image.open(out_wide).convert("RGBA"))
    assert int((wide[..., 1] > 0).sum()) >= int((narrow[..., 1] > 0).sum())


def test_cook_pak_and_fit_untouched(synthetic_island, tmp_path):
    pak_before = synthetic_island["pak"].read_bytes()
    fit_before = synthetic_island["fit"].read_text()
    out = tmp_path / "ro.png"
    run("cook", "--pak", str(synthetic_island["pak"]), "--out", str(out))
    assert synthetic_island["pak"].read_bytes() == pak_before
    assert synthetic_island["fit"].read_text() == fit_before


# --- cook: real stock mission (skip-if-missing) ------------------------------
@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_17.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_cook_mc2_17_has_water(tmp_path):
    out = tmp_path / "mc2_17_shoreline.png"
    r = run("cook", "--pak", str(CARVER5_MISSIONS / "mc2_17.pak"), "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "water_cells=" in r.stdout
    water_cells = int(r.stdout.split("water_cells=")[1].split()[0])
    assert water_cells > 0, "expected mc2_17 (the smear-reference water mission) to have water cells"


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_cook_mc2_24_deterministic(tmp_path):
    out1 = tmp_path / "a.png"
    out2 = tmp_path / "b.png"
    r1 = run("cook", "--pak", str(CARVER5_MISSIONS / "mc2_24.pak"), "--out", str(out1))
    r2 = run("cook", "--pak", str(CARVER5_MISSIONS / "mc2_24.pak"), "--out", str(out2))
    assert r1.returncode == 0, r1.stdout + r1.stderr
    assert r2.returncode == 0, r2.stdout + r2.stderr
    assert np.array_equal(np.array(Image.open(out1)), np.array(Image.open(out2)))


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
    Image.new("RGBA", (32, 32), (128, 0, 0, 255)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 0
    assert "WARN" in r.stdout


def test_validate_bad_bounds_fails(tmp_path):
    png = tmp_path / "solo.png"
    Image.new("RGBA", (32, 32), (128, 0, 0, 255)).save(png)
    bounds = tmp_path / "solo.bounds.txt"
    bounds.write_text("1.0 2.0 -5.0 3.0\n")  # negative sizeX
    r = run("validate", "--png", str(png), "--bounds", str(bounds))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_good_bounds_passes(tmp_path):
    png = tmp_path / "solo.png"
    Image.new("RGBA", (32, 32), (128, 0, 0, 255)).save(png)
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
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_17")
    assert r.returncode == 0, r.stdout + r.stderr
    dest = deploy / "data" / "missions" / "mc2_17.beauty" / "shoreline_mask.png"
    assert dest.is_file()
    dest_bounds = deploy / "data" / "missions" / "mc2_17.beauty" / "shoreline_mask.bounds.txt"
    assert dest_bounds.is_file()
    assert dest_bounds.read_text() == bounds.read_text()


def test_install_without_bounds_warns(tmp_path):
    png = tmp_path / "src_nobounds.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    Image.fromarray(rgba, "RGBA").save(png)
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_17")
    assert r.returncode == 0
    assert "WARN" in r.stdout


# --- all-missions batch (SHORELINE-BATCH-COOK-1) -----------------------------
@pytest.fixture()
def synthetic_missions_dir(tmp_path):
    """A missions dir with two synthetic maps: one water-bearing island and one
    all-land (dry) map that the batch must skip. Mirrors the real batch's water
    detection ([Water].Elevation vs coarse elev, never the .water byte)."""
    mdir = tmp_path / "missions"
    mdir.mkdir()
    # wet: island radius 12 at water_elev 0 -> surrounding cells go negative -> water
    _write_synthetic_pak(mdir / "wet_map.pak", 60, water_elev=0.0, island_radius=12.0)
    _write_fit(mdir / "wet_map.fit", 0.0)
    # dry: island radius huge so every cell is above the (very low) water level
    _write_synthetic_pak(mdir / "dry_map.pak", 60, water_elev=-1e6, island_radius=12.0)
    _write_fit(mdir / "dry_map.fit", -1e6)
    return mdir


def test_all_missions_cooks_water_skips_dry(synthetic_missions_dir, tmp_path):
    out_root = tmp_path / "out"
    census = tmp_path / "census.json"
    # foam-width 24: this synthetic dir has no <stem>.beauty bake, so cook falls
    # back to the coarse 128wu pak grid (16wu cells at ss=8) -- a 6wu foam band
    # can't land on that (same coarse-grid landmine the real batch avoids by
    # baking a 4x hi-res source first). Widen foam so the census row is nonzero;
    # the real batch keeps the 6wu default against its hi-res bakes.
    r = run("all-missions", "--missions-dir", str(synthetic_missions_dir),
            "--out-root", str(out_root), "--supersample", "8",
            "--foam-width", "24", "--census", str(census))
    assert r.returncode == 0, r.stdout + r.stderr
    # wet map cooked; dry map skipped as "no water cells"
    assert (out_root / "wet_map.beauty" / "shoreline_mask.png").is_file()
    assert (out_root / "wet_map.beauty" / "shoreline_mask.bounds.txt").is_file()
    assert not (out_root / "dry_map.beauty").exists()
    import json as _json
    doc = _json.loads(census.read_text())
    cooked = {c["mission"] for c in doc["cooked"]}
    skipped = {s["mission"]: s["reason"] for s in doc["skipped"]}
    assert cooked == {"wet_map"}
    assert skipped.get("dry_map") == "no water cells"
    # the cooked census row carries populated bands (supersample fix)
    row = next(c for c in doc["cooked"] if c["mission"] == "wet_map")
    assert row["wet_cells"] > 0 and row["foam_cells"] > 0


def test_all_missions_deterministic(synthetic_missions_dir, tmp_path):
    o1 = tmp_path / "o1"; o2 = tmp_path / "o2"
    r1 = run("all-missions", "--missions-dir", str(synthetic_missions_dir),
             "--out-root", str(o1), "--supersample", "8")
    r2 = run("all-missions", "--missions-dir", str(synthetic_missions_dir),
             "--out-root", str(o2), "--supersample", "8")
    assert r1.returncode == 0 and r2.returncode == 0
    a = np.array(Image.open(o1 / "wet_map.beauty" / "shoreline_mask.png"))
    b = np.array(Image.open(o2 / "wet_map.beauty" / "shoreline_mask.png"))
    assert np.array_equal(a, b)
