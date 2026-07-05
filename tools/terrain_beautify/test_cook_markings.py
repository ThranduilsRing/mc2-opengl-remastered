#!/usr/bin/env python3
"""pytest for cook_markings.py v3 (per-tile vector twins) + marking_glyphs.py.

Covers: overlay handle decode (offline tile-id reconstruction), cement-region
DETECTION (report only -- the "no legacy paint -> no vector paint" invariant),
paint-mask extraction, primitive fit + IoU gate, twin build fallbacks, and the
extract/twins/compose/install round-trip. marking_glyphs.py keeps its library
tests (the glyph DSL remains a standalone authoring tool)."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOL = HERE / "cook_markings.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")
STOCK_OVERLAYS = Path("A:/Games/mc2-opengl-src/mc2srcdata/textures/64Overlays")

sys.path.insert(0, str(HERE))
import marking_glyphs as mg  # noqa: E402
import cook_markings as cm  # noqa: E402


def run(*args) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True,
    )


# --- glyph library (marking_glyphs.py, standalone authoring DSL) ---------------
@pytest.mark.parametrize("name", sorted(mg.GLYPHS))
def test_glyph_deterministic(name):
    kwargs = {"text": "27L", "height_wu": 6.0} if name == "runway_numeral" else {}
    a1 = mg.render_glyph(name, 8.0, "white", **kwargs)
    a2 = mg.render_glyph(name, 8.0, "white", **kwargs)
    assert np.array_equal(a1, a2)


@pytest.mark.parametrize("name", sorted(mg.GLYPHS))
def test_glyph_alpha_native_rgba(name):
    kwargs = {"text": "09", "height_wu": 6.0} if name == "runway_numeral" else {}
    arr = mg.render_glyph(name, 8.0, "white", **kwargs)
    assert arr.dtype == np.uint8
    assert arr.ndim == 3 and arr.shape[2] == 4
    assert int(arr[..., 3].max()) > 0, "glyph must draw at least one opaque-ish pixel"


def test_glyph_rejects_non_white_yellow_color():
    with pytest.raises(ValueError):
        mg.render_glyph("road_centerline_dash", 8.0, "magenta")


def test_glyph_unknown_name_rejected():
    with pytest.raises(ValueError):
        mg.render_glyph("not_a_real_glyph", 8.0, "white")


# --- overlay handle decode ------------------------------------------------------
def test_decode_family_bases():
    """Slot allocation mirrors TerrainTextures::init: 255 terrain-type slots,
    then OverlayType0..16 in textures.fit order."""
    assert cm.decode_overlay_handle(255) == ("dirtroad", 0)
    assert cm.decode_overlay_handle(269) == ("dirtroad", 14)
    assert cm.decode_overlay_handle(270) == ("pavedroad", 0)
    # runway base = 255 + 15+15+8+4+2+15+15 = 329
    assert cm.decode_overlay_handle(329) == ("runway", 0)
    assert cm.decode_overlay_handle(343) == ("runway", 14)
    # bridge base = 255 + 89 + 6*50 = 644
    assert cm.decode_overlay_handle(644) == ("bridge", 0)
    assert cm.decode_overlay_handle(643) == ("x_2lanedirt_damaged", 49)


def test_decode_out_of_range_is_none():
    assert cm.decode_overlay_handle(0) is None
    assert cm.decode_overlay_handle(254) is None
    # past bridgedam end: 644 + 15 + 50 + 15 = 724
    assert cm.decode_overlay_handle(724) is None


def test_bridge_families_marked_runtime_mutable():
    assert cm.BRIDGE_FAMILIES == {"bridge", "x_pavedroad_bridge", "bridgedam"}


# --- cement-region classifier (DETECTION/REPORT ONLY) ---------------------------
def _diagonal_strip_mask(side: int = 64, half_width: int = 2,
                         r0: int = 10, r1: int = 44) -> np.ndarray:
    mask = np.zeros((side, side), dtype=bool)
    for r in range(r0, r1 + 1):
        for c in range(side):
            if abs(r - c) <= half_width:
                mask[r, c] = True
    return mask


def test_cement_classifier_diagonal_strip_is_runway():
    comps = cm._cement_components(_diagonal_strip_mask(), side=64)
    assert len(comps) == 1
    comp = comps[0]
    assert comp["kind"] == "cement_runway", comp
    assert abs(abs(comp["angle_deg"]) - 45.0) < 6.0, comp["angle_deg"]
    assert comp["length_wu"] > 30 * 128


def test_cement_classifier_square_block_is_pad():
    mask = np.zeros((64, 64), dtype=bool)
    mask[20:32, 20:32] = True
    comps = cm._cement_components(mask, side=64)
    assert len(comps) == 1
    assert comps[0]["kind"] == "cement_pad", comps[0]


def test_cement_classifier_ignores_tiny_blobs():
    mask = np.zeros((64, 64), dtype=bool)
    mask[5:8, 5:8] = True
    assert cm._cement_components(mask, side=64) == []


def test_cement_report_carries_no_paint_fields():
    """The invariant is structural: cement records are geometry reports only
    -- no glyphs, no colors, nothing a composer could stamp."""
    comps = cm._cement_components(_diagonal_strip_mask(), side=64)
    for comp in comps:
        assert set(comp.keys()) <= {"kind", "world_cx", "world_cy", "angle_deg",
                                    "length_wu", "width_wu", "count", "core_elong"}


# --- paint mask + twin build -----------------------------------------------------
def _synthetic_tile(dash: bool = True, yellow: bool = False) -> np.ndarray:
    """256px synthetic tile: dark asphalt body (lum 62) with an optional
    brighter dash (lum ~122, the measured legacy contrast)."""
    ref = np.zeros((256, 256, 4), dtype=np.uint8)
    ref[..., 0] = 62
    ref[..., 1] = 62
    ref[..., 2] = 60
    ref[..., 3] = 255
    if dash:
        if yellow:
            ref[120:136, 40:216, 0] = 130
            ref[120:136, 40:216, 1] = 115
            ref[120:136, 40:216, 2] = 60
        else:
            ref[120:136, 40:216, 0] = 122
            ref[120:136, 40:216, 1] = 122
            ref[120:136, 40:216, 2] = 120
    return ref


def test_paint_mask_isolates_bright_dash():
    got = cm.extract_paint_mask(_synthetic_tile())
    assert got is not None
    mask, body = got
    assert mask[128, 128]
    assert not mask[20, 20]
    assert 2000 < int(mask.sum()) < 4000  # ~16x176 dash


def test_paint_mask_detects_yellow():
    got = cm.extract_paint_mask(_synthetic_tile(yellow=True))
    assert got is not None
    mask, _ = got
    assert mask[128, 128]


def test_paint_mask_none_for_unpainted_tile():
    """INVARIANT: no legacy paint -> no vector paint."""
    assert cm.extract_paint_mask(_synthetic_tile(dash=False)) is None


def test_build_twin_vectorizes_clean_dash():
    got = cm.build_twin(_synthetic_tile())
    assert got is not None
    twin, stats = got
    assert stats["vector"] >= 1
    assert stats["raster"] == 0
    assert stats["tile_iou"] >= cm.TILE_IOU_ACCEPT
    # twin paint sits where the dash was, with the dash's own color
    assert twin[128, 128, 3] == 255
    assert abs(int(twin[128, 128, 0]) - 122) < 8
    assert twin[20, 20, 3] == 0


def test_build_twin_none_for_unpainted_tile():
    assert cm.build_twin(_synthetic_tile(dash=False)) is None


def test_build_twin_raster_fallback_for_complex_shape():
    """A shape a capsule/rect cannot fit (a ring) must fall back to the
    reference raster pixels rather than shipping a bad vector."""
    ref = _synthetic_tile(dash=False)
    yy, xx = np.mgrid[0:256, 0:256]
    d = np.sqrt((yy - 128) ** 2 + (xx - 128) ** 2)
    ring = (d > 60) & (d < 90)
    ref[ring, 0] = 122
    ref[ring, 1] = 122
    ref[ring, 2] = 120
    got = cm.build_twin(ref)
    assert got is not None
    twin, stats = got
    assert stats["raster"] >= 1
    assert stats["tile_iou"] >= cm.TILE_IOU_ACCEPT  # raster fallback keeps fidelity


def test_iou_helper():
    a = np.zeros((4, 4), dtype=bool)
    b = np.zeros((4, 4), dtype=bool)
    a[0, 0] = True
    b[0, 0] = True
    b[0, 1] = True
    assert cm._iou(a, b) == 0.5


# --- extract (real pak, skipif) --------------------------------------------------
def _extract(pak_stem: str, out: Path) -> subprocess.CompletedProcess:
    return run("extract", "--pak", str(CARVER5_MISSIONS / f"{pak_stem}.pak"), "--out", str(out))


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                    reason="Carver5-feasibility stock mission data not present")
def test_extract_mc2_24_decodes_runway_and_roads(tmp_path):
    out = tmp_path / "m.json"
    r = _extract("mc2_24", out)
    assert r.returncode == 0, r.stdout + r.stderr
    doc = json.loads(out.read_text())
    assert doc["unknown_overlay_values"] == 0
    fams = doc["family_histogram"]
    assert "runway" in fams and "pavedroad" in fams
    for cell in doc["cells"]:
        assert cell["family"] not in cm.BRIDGE_FAMILIES
        assert 0 <= cell["tile"] < dict(cm.OVERLAY_FAMILIES)[cell["family"]]


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                    reason="Carver5-feasibility stock mission data not present")
def test_extract_deterministic(tmp_path):
    out1 = tmp_path / "a.json"
    out2 = tmp_path / "b.json"
    r1 = _extract("mc2_24", out1)
    r2 = _extract("mc2_24", out2)
    assert r1.returncode == 0 and r2.returncode == 0
    assert out1.read_text() == out2.read_text()


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                    reason="Carver5-feasibility stock mission data not present")
def test_extract_mc2_01_finds_runway_tiles_and_cement_report(tmp_path):
    """mc2_01: the runway PAINT lives in runway#### overlay tiles (decoded +
    stamped); the CEMENT diamond is detection/report only (extraction-gap fix
    requirement 'runway found' -- but never a paint source)."""
    out = tmp_path / "m.json"
    r = _extract("mc2_01", out)
    assert r.returncode == 0, r.stdout + r.stderr
    doc = json.loads(out.read_text())
    assert "runway" in doc["family_histogram"]
    kinds = [x["kind"] for x in doc["cement_report"]]
    assert "cement_runway" in kinds
    assert "cement_runway_components=1" in r.stdout


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                    reason="Carver5-feasibility stock mission data not present")
def test_extract_pak_untouched_byte_identical(tmp_path):
    pak = CARVER5_MISSIONS / "mc2_01.pak"
    before = pak.read_bytes()
    _extract("mc2_01", tmp_path / "m.json")
    assert pak.read_bytes() == before


def test_extract_missing_pak_fails(tmp_path):
    r = run("extract", "--pak", str(tmp_path / "nope.pak"), "--out", str(tmp_path / "out.json"))
    assert r.returncode == 4


# --- twins CLI (real tile art, skipif; bicubic path for determinism) -------------
@pytest.mark.skipif(not (STOCK_OVERLAYS / "runway0011.tga").is_file(),
                    reason="mc2srcdata 64Overlays tile art not present")
def test_twins_builds_runway_stamp_and_report(tmp_path):
    """runway0011 carries real bright markings (threshold bars + numerals) ->
    a stamp is built. runway0000 is a plain taxiway with NO bright markings
    (only body gradient) -> under the marking-only contract it correctly
    reports no_paint and ships no stamp. dirtroad has no paint at all."""
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({
        "mission": "synthetic", "side": 100,
        "cells": [{"row": 1, "col": 1, "family": "runway", "tile": 11},
                  {"row": 1, "col": 3, "family": "runway", "tile": 0},
                  {"row": 1, "col": 2, "family": "dirtroad", "tile": 0}],
    }))
    out_dir = tmp_path / "twins"
    r = run("twins", "--markings", str(markings), "--tiles-dir", str(STOCK_OVERLAYS),
            "--out-dir", str(out_dir), "--no-esrgan")
    assert r.returncode == 0, r.stdout + r.stderr
    report = json.loads((out_dir / "twins_report.json").read_text())
    assert report["upscaler"] == "bicubic"
    # marking-bearing runway tile -> a stamp is built
    assert report["tiles"]["runway0011"]["status"] in ("ok", "below_threshold_raster_used")
    assert (out_dir / "runway0011.twin.png").is_file()
    # plain taxiway (no bright paint) and dirt road -> INVARIANT: no twin file
    assert report["tiles"]["runway0000"]["status"] == "no_paint"
    assert not (out_dir / "runway0000.twin.png").is_file()
    assert report["tiles"]["dirtroad0000"]["status"] == "no_paint"
    assert not (out_dir / "dirtroad0000.twin.png").is_file()
    twin = np.array(Image.open(out_dir / "runway0011.twin.png").convert("RGBA"))
    assert twin.shape[2] == 4 and int((twin[..., 3] > 0).sum()) > 0
    # STAMP-ONLY-MARKING: the twin opaque region must be bright paint, NOT the
    # dark asphalt body -- the whole point of the marking-only fix.
    op = twin[..., 3] > 0
    opaque_lum = twin[op][:, :3].astype(int).mean(axis=1)
    assert float(opaque_lum.min()) > 100, "stamp must not carry dark body pixels"


# --- compose ----------------------------------------------------------------------
def _make_overlay_png(tmp_path: Path, w: int, h: int) -> Path:
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., :3] = 150
    rgba[..., 3] = 255
    p = tmp_path / "base_overlay.png"
    Image.fromarray(rgba, "RGBA").save(p)
    # bounds matching a 4-cell (side=4) full map: half = 4*128/2 = 256
    (tmp_path / "base_overlay.bounds.txt").write_text("-256.0 256.0 512.0 512.0\n")
    return p


def _make_twin(dir_: Path, name: str) -> None:
    dir_.mkdir(parents=True, exist_ok=True)
    twin = np.zeros((64, 64, 4), dtype=np.uint8)
    twin[24:40, 8:56, 0] = 200
    twin[24:40, 8:56, 1] = 200
    twin[24:40, 8:56, 2] = 200
    twin[24:40, 8:56, 3] = 255
    Image.fromarray(twin, "RGBA").save(dir_ / f"{name}.twin.png")


def test_compose_stamps_twin_at_cell_footprint(tmp_path):
    overlay = _make_overlay_png(tmp_path, 64, 64)
    twins = tmp_path / "twins"
    _make_twin(twins, "runway0000")
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({
        "mission": "synthetic", "side": 4,
        "cells": [{"row": 1, "col": 2, "family": "runway", "tile": 0}],
    }))
    out = tmp_path / "composed.png"
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay),
            "--twins", str(twins), "--out", str(out), "--px-per-wu", "0.125")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "stamped=1" in r.stdout
    arr = np.array(Image.open(out).convert("RGBA"))
    assert arr.shape[:2] == (64, 64)  # 512wu * 0.125
    # cell (row=1,col=2) covers px x:[32,48) y:[16,32); twin dash center
    # lands mid-cell
    assert arr[24, 40, 0] > 180, arr[24, 40]
    # far corner untouched base grey
    assert tuple(arr[2, 2, :3]) == (150, 150, 150)


def test_compose_skips_cells_without_twin(tmp_path):
    overlay = _make_overlay_png(tmp_path, 64, 64)
    twins = tmp_path / "twins"
    twins.mkdir()
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({
        "mission": "synthetic", "side": 4,
        "cells": [{"row": 0, "col": 0, "family": "dirtroad", "tile": 3}],
    }))
    out = tmp_path / "composed.png"
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay),
            "--twins", str(twins), "--out", str(out), "--px-per-wu", "0.125")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "stamped=0" in r.stdout and "skipped_no_paint=1" in r.stdout
    arr = np.array(Image.open(out).convert("RGBA"))
    assert tuple(arr[8, 8, :3]) == (150, 150, 150)  # base untouched


def test_compose_requires_bounds(tmp_path):
    overlay = _make_overlay_png(tmp_path, 20, 20)
    (tmp_path / "base_overlay.bounds.txt").unlink()
    twins = tmp_path / "twins"
    twins.mkdir()
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({"mission": "x", "side": 4, "cells": []}))
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay),
            "--twins", str(twins), "--out", str(tmp_path / "c.png"))
    assert r.returncode == 4
    assert "ERROR" in r.stdout + r.stderr


def test_compose_missing_markings_fails(tmp_path):
    overlay = _make_overlay_png(tmp_path, 10, 10)
    twins = tmp_path / "twins"
    twins.mkdir()
    r = run("compose", "--markings", str(tmp_path / "nope.json"), "--overlay", str(overlay),
            "--twins", str(twins), "--out", str(tmp_path / "out.png"))
    assert r.returncode == 4


def test_compose_deterministic(tmp_path):
    overlay = _make_overlay_png(tmp_path, 64, 64)
    twins = tmp_path / "twins"
    _make_twin(twins, "pavedroad0001")
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({
        "mission": "synthetic", "side": 4,
        "cells": [{"row": 2, "col": 1, "family": "pavedroad", "tile": 1}],
    }))
    o1, o2 = tmp_path / "c1.png", tmp_path / "c2.png"
    r1 = run("compose", "--markings", str(markings), "--overlay", str(overlay),
             "--twins", str(twins), "--out", str(o1), "--px-per-wu", "0.125")
    r2 = run("compose", "--markings", str(markings), "--overlay", str(overlay),
             "--twins", str(twins), "--out", str(o2), "--px-per-wu", "0.125")
    assert r1.returncode == 0 and r2.returncode == 0
    assert o1.read_bytes() == o2.read_bytes()


def test_compose_base_upsample_is_nearest_no_feather(tmp_path):
    """ROOT-CAUSE regression (cement blobs): a coarse binary-alpha parity
    raster composed at a larger canvas must upsample NEAREST -- no partial
    alpha may appear along cement edges (that soft ramp rendered as translucent
    'fog cloud' halos). BICUBIC would introduce hundreds of partial-alpha
    texels here."""
    # 4x4 parity raster with a hard-edged cement block (alpha 0 or 255 only)
    small = np.zeros((4, 4, 4), dtype=np.uint8)
    small[..., :3] = 150
    small[1:3, 1:3, 3] = 255  # a 2x2 opaque cement pad, hard edges
    overlay = tmp_path / "base_overlay.png"
    Image.fromarray(small, "RGBA").save(overlay)
    (tmp_path / "base_overlay.bounds.txt").write_text("-256.0 256.0 512.0 512.0\n")
    twins = tmp_path / "twins"
    twins.mkdir()
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({"mission": "x", "side": 4, "cells": []}))
    out = tmp_path / "composed.png"
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay),
            "--twins", str(twins), "--out", str(out), "--px-per-wu", "1.0")  # 512px canvas
    assert r.returncode == 0, r.stdout + r.stderr
    arr = np.array(Image.open(out).convert("RGBA"))
    a = arr[..., 3]
    partial = ((a > 0) & (a < 255)).sum()
    assert partial == 0, f"cement edges feathered: {partial} partial-alpha texels (want 0)"


# --- whole-image acceptance gate ---------------------------------------------
def test_whole_image_gate_passes_when_diff_confined():
    base = np.zeros((20, 20, 4), dtype=np.uint8)
    base[..., :3] = 150
    base[..., 3] = 255
    composed = base.copy()
    footprint = np.zeros((20, 20), dtype=bool)
    footprint[8:12, 8:12] = True
    composed[9:11, 9:11, :3] = 255  # a marking, inside the footprint
    rep = cm.whole_image_diff_confined(composed, base, footprint)
    assert rep["pass"], rep


def test_whole_image_gate_fails_on_body_ship_outside_footprint():
    """The 'solid white runway' class: the parity base is TRANSPARENT outside
    the concrete cells; a stamp that ships opaque body pixels turns
    transparent texels opaque OUTSIDE the true marking footprint -> the gate
    must FAIL (the alpha 0->255 delta is what makes the runway read solid,
    even when the grey RGB nearly matches the base)."""
    base = np.zeros((20, 20, 4), dtype=np.uint8)
    base[..., :3] = 150  # RGB present but...
    base[..., 3] = 0     # ...fully transparent everywhere (no overlay here)
    composed = base.copy()
    footprint = np.zeros((20, 20), dtype=bool)
    footprint[8:12, 8:12] = True
    # a big opaque grey slab painted well outside the tiny footprint (body ship)
    composed[2:18, 2:18, :3] = 152
    composed[2:18, 2:18, 3] = 255
    rep = cm.whole_image_diff_confined(composed, base, footprint)
    assert not rep["pass"]
    assert rep["outside_footprint_texels"] > 0


def test_whole_image_gate_fails_on_feathered_base():
    """The 'cement blob' class: feathering the base alpha changes texels with
    NO stamp over them -> the gate must FAIL."""
    base = np.zeros((20, 20, 4), dtype=np.uint8)
    base[..., :3] = 150
    base[5:15, 5:15, 3] = 255  # hard cement block
    composed = base.copy()
    footprint = np.zeros((20, 20), dtype=bool)  # no stamps at all
    # simulate a bicubic feather: soft alpha ramp around the block edge
    composed[4, 5:15, 3] = 120
    composed[15, 5:15, 3] = 120
    rep = cm.whole_image_diff_confined(composed, base, footprint)
    assert not rep["pass"]
    assert rep["outside_footprint_texels"] > 0


def test_whole_image_gate_allows_aa_margin():
    """A 1px LANCZOS AA fringe just past the recorded footprint is tolerated by
    the AA margin (not a body-ship error)."""
    base = np.zeros((20, 20, 4), dtype=np.uint8)
    base[..., :3] = 150
    base[..., 3] = 255
    composed = base.copy()
    footprint = np.zeros((20, 20), dtype=bool)
    footprint[8:12, 8:12] = True
    composed[7:13, 7:13, :3] = 255  # marking + 1px AA fringe past footprint
    rep = cm.whole_image_diff_confined(composed, base, footprint, aa_margin=2)
    assert rep["pass"], rep


def test_verify_cli_passes_on_clean_compose(tmp_path):
    """compose -> verify round-trip: a clean marking-only stamp passes."""
    overlay = _make_overlay_png(tmp_path, 64, 64)
    twins = tmp_path / "twins"
    _make_twin(twins, "runway0000")  # small central dash, marking-only
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({
        "mission": "synthetic", "side": 4,
        "cells": [{"row": 1, "col": 2, "family": "runway", "tile": 0}],
    }))
    composed = tmp_path / "composed.png"
    rc = run("compose", "--markings", str(markings), "--overlay", str(overlay),
             "--twins", str(twins), "--out", str(composed), "--px-per-wu", "0.125")
    assert rc.returncode == 0, rc.stdout + rc.stderr
    rv = run("verify", "--markings", str(markings), "--overlay", str(overlay),
             "--twins", str(twins), "--composed", str(composed))
    assert rv.returncode == 0, rv.stdout + rv.stderr
    assert "PASS" in rv.stdout


def test_verify_cli_fails_on_altered_base(tmp_path):
    """If the composed sidecar's base surface was altered outside stamps, the
    whole-image gate FAILs (exit 1). This is the check the per-tile IoU gate
    could not make."""
    overlay = _make_overlay_png(tmp_path, 64, 64)
    twins = tmp_path / "twins"
    _make_twin(twins, "runway0000")
    markings = tmp_path / "m.json"
    markings.write_text(json.dumps({
        "mission": "synthetic", "side": 4,
        "cells": [{"row": 1, "col": 2, "family": "runway", "tile": 0}],
    }))
    composed = tmp_path / "composed.png"
    rc = run("compose", "--markings", str(markings), "--overlay", str(overlay),
             "--twins", str(twins), "--out", str(composed), "--px-per-wu", "0.125")
    assert rc.returncode == 0
    # corrupt the composed image far from the stamp (simulate body-ship/feather)
    arr = np.array(Image.open(composed).convert("RGBA"))
    arr[0:20, 0:20, :3] = 200
    Image.fromarray(arr, "RGBA").save(composed)
    rv = run("verify", "--markings", str(markings), "--overlay", str(overlay),
             "--twins", str(twins), "--composed", str(composed))
    assert rv.returncode == 1, rv.stdout + rv.stderr
    assert "FAIL" in rv.stdout + rv.stderr


# --- install --------------------------------------------------------------
def test_install_copies_to_beauty_dir(tmp_path):
    png = tmp_path / "src.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    Image.fromarray(rgba, "RGBA").save(png)
    (tmp_path / "src.bounds.txt").write_text("-100.0 100.0 200.0 200.0\n")
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_24")
    assert r.returncode == 0, r.stdout + r.stderr
    assert (deploy / "data" / "missions" / "mc2_24.beauty" / "overlay_v2.png").is_file()
    assert (deploy / "data" / "missions" / "mc2_24.beauty" / "overlay_v2.bounds.txt").is_file()


def test_install_missing_source_fails(tmp_path):
    r = run("install", "--png", str(tmp_path / "nope.png"), "--deploy", str(tmp_path / "deploy"),
            "--mission", "mc2_24")
    assert r.returncode == 4


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
