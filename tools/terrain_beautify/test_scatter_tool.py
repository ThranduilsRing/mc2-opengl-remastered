#!/usr/bin/env python3
"""pytest for scatter_tool.py (TERRAIN-SCATTER-MASK-1).

Covers: plan determinism (subprocess, PYTHONHASHSEED-proof), rejection gates
on synthetic masks, cook-refusal matrix, and round-trip prior-packet
byte-identity on a copy of a stock .pak.
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
TOOL = HERE / "scatter_tool.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")

sys.path.insert(0, str(HERE))
import scatter_tool as st  # noqa: E402

sys.path.insert(0, str(HERE.parent))
import pak_append  # noqa: E402


def run(*args, env=None) -> subprocess.CompletedProcess:
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True, env=full_env,
    )


# --- determinism (subprocess, guards against PYTHONHASHSEED dict-order bugs) -
@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(), reason="Carver5 fixture pak not present")
def test_plan_deterministic_same_seed(tmp_path):
    out1 = tmp_path / "run1"
    out2 = tmp_path / "run2"
    common = ["plan", "mc2_01", "--missions-dir", str(CARVER5_MISSIONS), "--count", "30", "--seed", "7"]
    r1 = run(*common, "--out", str(out1), env={"PYTHONHASHSEED": "1"})
    r2 = run(*common, "--out", str(out2), env={"PYTHONHASHSEED": "42"})
    assert r1.returncode == 0, r1.stderr
    assert r2.returncode == 0, r2.stderr
    m1 = json.loads((out1 / "mc2_01.scatter_manifest.json").read_text())
    m2 = json.loads((out2 / "mc2_01.scatter_manifest.json").read_text())
    # baseHash/source_pak are environment-independent; placements must match
    # byte-for-byte regardless of PYTHONHASHSEED.
    assert m1["placements"] == m2["placements"]
    assert m1["rejects"] == m2["rejects"]
    assert m1["count_final"] == m2["count_final"]


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(), reason="Carver5 fixture pak not present")
def test_plan_different_seed_differs(tmp_path):
    out1 = tmp_path / "s1"
    out2 = tmp_path / "s2"
    common = ["plan", "mc2_01", "--missions-dir", str(CARVER5_MISSIONS), "--count", "30"]
    run(*common, "--seed", "1", "--out", str(out1))
    run(*common, "--seed", "2", "--out", str(out2))
    m1 = json.loads((out1 / "mc2_01.scatter_manifest.json").read_text())
    m2 = json.loads((out2 / "mc2_01.scatter_manifest.json").read_text())
    assert m1["placements"] != m2["placements"]


# --- rejection gates on synthetic masks --------------------------------------
def _synthetic_grid(side=20):
    elev = np.zeros((side, side), dtype=np.float64)
    for r in range(side):
        elev[r, :] = r * 5.0  # simple ramp -> nonzero slope everywhere
    return elev


def test_water_exclusion_rejects_all_candidates():
    side = 10
    elev = np.zeros((side, side))
    density = np.ones((side, side))
    exclusion = {
        "water": np.ones((side, side), dtype=bool),
        "overlay": np.zeros((side, side), dtype=bool),
        "building_footprint": np.zeros((side, side), dtype=bool),
        "slope": np.zeros((side, side), dtype=bool),
    }
    result = st.plan_placements(side, elev, density, exclusion, count=50, seed=1, min_dist_wu=1.0)
    assert len(result["accepted"]) == 0
    assert result["rejects"]["water"] == side * side


def test_slope_gate_rejects_steep_cells():
    side = 10
    elev = _synthetic_grid(side)
    density = np.ones((side, side))
    exclusion = {
        "water": np.zeros((side, side), dtype=bool),
        "overlay": np.zeros((side, side), dtype=bool),
        "building_footprint": np.zeros((side, side), dtype=bool),
        "slope": np.zeros((side, side), dtype=bool),
    }
    exclusion["slope"][5:, :] = True  # bottom half too steep
    # count must exceed the eligible (top-half) cell count so the walk keeps
    # going into the excluded bottom half and actually tallies those rejects,
    # rather than stopping early once `count` accepted cells are found.
    result = st.plan_placements(side, elev, density, exclusion, count=side * side, seed=3, min_dist_wu=1.0)
    for p in result["accepted"]:
        assert p["row"] < 5
    assert result["rejects"]["slope"] == 5 * side


def test_min_distance_thins_dense_candidates():
    side = 20
    elev = np.zeros((side, side))
    density = np.ones((side, side))
    exclusion = {
        "water": np.zeros((side, side), dtype=bool),
        "overlay": np.zeros((side, side), dtype=bool),
        "building_footprint": np.zeros((side, side), dtype=bool),
        "slope": np.zeros((side, side), dtype=bool),
    }
    # Very large min-distance should force heavy spacing rejection and a
    # small accepted set even though every cell passes density_roll (weight 1).
    result = st.plan_placements(side, elev, density, exclusion, count=400, seed=1,
                                 min_dist_wu=1000.0)
    assert len(result["accepted"]) < 10
    assert result["rejects"]["spacing"] > 0


def test_count_cap_stops_early():
    side = 30
    elev = np.zeros((side, side))
    density = np.ones((side, side))
    exclusion = {
        "water": np.zeros((side, side), dtype=bool),
        "overlay": np.zeros((side, side), dtype=bool),
        "building_footprint": np.zeros((side, side), dtype=bool),
        "slope": np.zeros((side, side), dtype=bool),
    }
    result = st.plan_placements(side, elev, density, exclusion, count=5, seed=1, min_dist_wu=1.0)
    assert len(result["accepted"]) == 5


# --- cook-refusal matrix ------------------------------------------------------
def _make_synthetic_pak_with_objects(tmp_path: Path, side=60, num_objs=2) -> Path:
    """Minimal synthetic .pak: packet 0 = MapData-shaped RAW blob (not
    strictly required by cook, only by plan/analyzer), packet 1 = RAW
    terrain-objects packet with `num_objs` pre-existing (hand-placed) records
    of objType 999 (outside RESERVED_SCATTER_OBJTYPENUMS)."""
    pcv_size = 32
    map_blob = bytes(pcv_size * side * side)  # zeroed PostcompVertex array
    records = struct.pack('<i', num_objs)
    for i in range(num_objs):
        records += struct.pack('<i4f5i', 999, float(i * 10), float(i * 10), 0.0, 0.0, 0, -1, -1, 0, 0)
    packets = [
        pak_append.PacketRecord(0, pak_append.ST_RAW, map_blob),
        pak_append.PacketRecord(1, pak_append.ST_RAW, records),
    ]
    out = tmp_path / "synthetic_scatter.pak"
    pak_append.write_pak(packets, out)
    return out


def _make_manifest(tmp_path: Path, pak_path: Path, side, objtypenum, placements, base_hash=None) -> Path:
    manifest = {
        "format": "mc2-scatter-manifest/1",
        "mission": "synthetic",
        "source_pak": str(pak_path),
        "baseHash": base_hash if base_hash is not None else st.sha256_file(pak_path),
        "grid_side": side,
        "seed": 1,
        "count_requested": len(placements),
        "count_final": len(placements),
        "objTypeNum": objtypenum,
        "thresholds": {},
        "rejects": {},
        "candidates_visited": len(placements),
        "placements": placements,
    }
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def _sample_placements(n):
    return [{"row": i, "col": i, "x": float(i * 20), "y": float(i * 20), "z": 0.0, "yaw": 0.0}
            for i in range(n)]


def test_cook_refuses_without_verify_flag(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    manifest = _make_manifest(tmp_path, pak_path, 60, st.MARBLECLIFF_OBJTYPENUM, _sample_placements(3))
    with pytest.raises(ValueError, match="prop-verified"):
        st.cook_scatter(pak_path, manifest, tmp_path / "out.pak",
                         prop_verified=False, allow_unverified=False)


def test_cook_refuses_wrong_objtypenum(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    manifest = _make_manifest(tmp_path, pak_path, 60, 42, _sample_placements(3))
    with pytest.raises(ValueError, match="RESERVED_SCATTER_OBJTYPENUMS"):
        st.cook_scatter(pak_path, manifest, tmp_path / "out.pak",
                         prop_verified=True, allow_unverified=False)


def test_cook_refuses_hash_mismatch(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    manifest = _make_manifest(tmp_path, pak_path, 60, st.MARBLECLIFF_OBJTYPENUM,
                               _sample_placements(3), base_hash="0" * 64)
    with pytest.raises(ValueError, match="baseHash mismatch"):
        st.cook_scatter(pak_path, manifest, tmp_path / "out.pak",
                         prop_verified=True, allow_unverified=False)


def test_cook_refuses_over_cap(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    manifest = _make_manifest(tmp_path, pak_path, 60, st.MARBLECLIFF_OBJTYPENUM,
                               _sample_placements(st.HARD_CAP + 1))
    with pytest.raises(ValueError, match="HARD_CAP"):
        st.cook_scatter(pak_path, manifest, tmp_path / "out.pak",
                         prop_verified=True, allow_unverified=False)


def test_cook_succeeds_with_allow_unverified(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path, num_objs=2)
    manifest = _make_manifest(tmp_path, pak_path, 60, st.MARBLECLIFF_OBJTYPENUM, _sample_placements(3))
    out_pak = tmp_path / "cooked.pak"
    stats = st.cook_scatter(pak_path, manifest, out_pak, prop_verified=False, allow_unverified=True)
    assert stats["new_scatter_written"] == 3
    assert stats["prior_scatter_removed"] == 0
    assert stats["final_object_count"] == 2 + 3  # 2 hand-placed (type 999) survive
    assert out_pak.is_file()


def test_cook_never_mutates_stock_in_place(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    original_bytes = pak_path.read_bytes()
    manifest = _make_manifest(tmp_path, pak_path, 60, st.MARBLECLIFF_OBJTYPENUM, _sample_placements(2))
    st.cook_scatter(pak_path, manifest, tmp_path / "out.pak", prop_verified=True, allow_unverified=False)
    assert pak_path.read_bytes() == original_bytes


# --- re-cook idempotency ------------------------------------------------------
def test_recook_is_idempotent_delete_and_reemit(tmp_path):
    pak_path = _make_synthetic_pak_with_objects(tmp_path, num_objs=1)
    manifest_a = _make_manifest(tmp_path, pak_path, 60, st.MARBLECLIFF_OBJTYPENUM, _sample_placements(3))
    out_a = tmp_path / "cooked_a.pak"
    stats_a = st.cook_scatter(pak_path, manifest_a, out_a, prop_verified=True, allow_unverified=False)
    assert stats_a["final_object_count"] == 1 + 3

    # Re-cook FROM THE COOKED PAK with a manifest whose baseHash matches the
    # cooked pak, and a different placement set -- prior scatter records (our
    # reserved type) must be removed before the new set is written, so the
    # hand-placed record (type 999) survives and the scatter set is replaced,
    # not appended-on-top-of.
    manifest_b = _make_manifest(tmp_path, out_a, 60, st.MARBLECLIFF_OBJTYPENUM, _sample_placements(5))
    out_b = tmp_path / "cooked_b.pak"
    stats_b = st.cook_scatter(out_a, manifest_b, out_b, prop_verified=True, allow_unverified=False)
    assert stats_b["prior_scatter_removed"] == 3
    assert stats_b["new_scatter_written"] == 5
    assert stats_b["final_object_count"] == 1 + 5


# --- tree channel (TERRAIN-TREE-SCATTER-V0) ----------------------------------
def test_tree_slope_gate_excludes_outside_band():
    """Tree channel's exclusion is the recon's 'slope-gate flip': rock
    excludes ONLY too-steep; tree excludes BOTH too-flat and too-steep,
    keeping only the [MIN,MAX] degree band."""
    side = 10
    layers = {"water": np.zeros((side, side), dtype=bool),
              "overlay": np.zeros((side, side), dtype=bool)}
    masks = {"slope_deg": np.array([[0.0, 10.0, 15.0, 20.0, 30.0] * 2] * side)}
    foot = np.zeros((side, side), dtype=bool)
    excl = st.compute_exclusion(layers, masks, foot, channel="tree")
    # col 0 (slope=0) and col 4 (slope=30) are outside [5,25] -> excluded.
    # col 1/2/3 (10/15/20) are inside -> not excluded by slope.
    assert excl["slope"][:, 0].all()
    assert excl["slope"][:, 4].all()
    assert not excl["slope"][:, 1].any()
    assert not excl["slope"][:, 2].any()
    assert not excl["slope"][:, 3].any()


def test_rock_slope_gate_unchanged_by_channel_param():
    """Default/rock channel must behave exactly as before (only >MAX
    excluded) -- guards against the tree flip leaking into the rock path."""
    side = 10
    layers = {"water": np.zeros((side, side), dtype=bool),
              "overlay": np.zeros((side, side), dtype=bool)}
    masks = {"slope_deg": np.array([[0.0, 10.0, 41.0] * 3 + [0.0]] * side)}
    foot = np.zeros((side, side), dtype=bool)
    excl = st.compute_exclusion(layers, masks, foot, channel="rock")
    assert not excl["slope"][:, 0].any()  # flat: fine for rock
    assert excl["slope"][:, 2].any()  # 41 deg > DEFAULT_SLOPE_MAX_DEG(40): excluded


def test_water_shore_concrete_exclusion_unchanged_across_channels():
    """Recon requirement: 'water/shore/concrete exclusion stays' regardless
    of channel -- only the slope gate differs."""
    side = 6
    layers = {"water": np.array([[True] * side] + [[False] * side] * (side - 1)),
              "overlay": np.array([[False] * side, [True] * side] + [[False] * side] * (side - 2))}
    masks = {"slope_deg": np.zeros((side, side))}
    foot = np.zeros((side, side), dtype=bool)
    for channel in ("rock", "tree"):
        excl = st.compute_exclusion(layers, masks, foot, channel=channel)
        assert excl["water"][0].all()
        assert excl["overlay"][1].all()


def test_tree_density_peaks_mid_band_and_zero_outside():
    side = 5
    masks = {"slope_deg": np.array([[0.0, 5.0, 15.0, 25.0, 40.0]] * side),
             "flat_playable": np.zeros((side, side), dtype=bool)}
    density = st.load_density_mask(side, masks, None, channel="tree")
    # mid-band (15 deg, the midpoint of [5,25]) has the highest density in
    # the row; 0 deg and 40 deg (well outside the band) are at/near zero.
    row = density[0]
    assert row[2] == pytest.approx(max(row), abs=1e-9)
    assert row[0] == pytest.approx(0.0, abs=1e-9)
    assert row[4] == pytest.approx(0.0, abs=1e-9)


def test_plan_placements_tree_pool_assigns_reserved_objtypenums():
    """Multi-prop pool: every accepted placement gets an objTypeNum key
    drawn from the pool, and (since PYTHONHASHSEED-proof _cell_rng is
    deterministic) re-running with the same seed reproduces the same picks."""
    side = 12
    elev = np.zeros((side, side))
    density = np.ones((side, side))
    exclusion = {"water": np.zeros((side, side), dtype=bool),
                 "overlay": np.zeros((side, side), dtype=bool),
                 "building_footprint": np.zeros((side, side), dtype=bool),
                 "slope": np.zeros((side, side), dtype=bool)}
    pool = st.PINE_SCATTER_OBJTYPENUMS
    result_a = st.plan_placements(side, elev, density, exclusion, count=20, seed=9,
                                   min_dist_wu=1.0, prop_pool=pool)
    result_b = st.plan_placements(side, elev, density, exclusion, count=20, seed=9,
                                   min_dist_wu=1.0, prop_pool=pool)
    assert len(result_a["accepted"]) == 20
    for p in result_a["accepted"]:
        assert p["objTypeNum"] in pool
    assert [p["objTypeNum"] for p in result_a["accepted"]] == \
           [p["objTypeNum"] for p in result_b["accepted"]]
    # sanity: with 20 draws across a 3-item pool, expect more than one
    # distinct type picked (not a hardcoded constant pick).
    assert len({p["objTypeNum"] for p in result_a["accepted"]}) > 1


def test_plan_placements_single_prop_channel_has_no_objtypenum_key():
    """Rock (single-prop, prop_pool=None) must NOT add a per-placement
    objTypeNum key -- preserves exact pre-existing manifest shape."""
    side = 8
    elev = np.zeros((side, side))
    density = np.ones((side, side))
    exclusion = {"water": np.zeros((side, side), dtype=bool),
                 "overlay": np.zeros((side, side), dtype=bool),
                 "building_footprint": np.zeros((side, side), dtype=bool),
                 "slope": np.zeros((side, side), dtype=bool)}
    result = st.plan_placements(side, elev, density, exclusion, count=5, seed=1,
                                 min_dist_wu=1.0, prop_pool=None)
    for p in result["accepted"]:
        assert "objTypeNum" not in p


def test_cook_accepts_mixed_pine_scatter_objtypenums(tmp_path):
    """cook_scatter must honor per-placement objTypeNum for the tree channel
    (mixed PineScatter1/2/3 in one manifest) and validate ALL of them against
    RESERVED_SCATTER_OBJTYPENUMS, not just a single manifest-level value."""
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    pool = st.PINE_SCATTER_OBJTYPENUMS
    placements = [
        {"row": i, "col": i, "x": float(i * 300), "y": float(i * 300), "z": 0.0,
         "yaw": 0.0, "objTypeNum": pool[i % len(pool)]}
        for i in range(6)
    ]
    manifest = _make_manifest(tmp_path, pak_path, 60, pool[0], placements)
    out_pak = tmp_path / "cooked_tree.pak"
    stats = st.cook_scatter(pak_path, manifest, out_pak, prop_verified=False, allow_unverified=True)
    assert stats["new_scatter_written"] == 6

    after = pak_append.read_packets(out_pak)
    from mission_terrain_analyzer import decode_packet as _decode
    for rec in after:
        d = _decode(rec.storage_type, rec.payload)
        if d is None or len(d) < 4:
            continue
        count = struct.unpack_from('<i', d, 0)[0]
        if count <= 0 or 4 + count * 40 != len(d):
            continue
        recs = st._read_records(d)
        written_types = {r[0] for r in recs if r[0] in pool}
        assert written_types == set(pool[i % len(pool)] for i in range(6))
        break


def test_cook_refuses_mixed_manifest_with_one_bad_objtypenum(tmp_path):
    """One rogue objTypeNum among otherwise-valid tree placements must still
    refuse the whole cook (exhaustive validation, not just the first/last)."""
    pak_path = _make_synthetic_pak_with_objects(tmp_path)
    pool = st.PINE_SCATTER_OBJTYPENUMS
    placements = [
        {"row": 0, "col": 0, "x": 0.0, "y": 0.0, "z": 0.0, "yaw": 0.0, "objTypeNum": pool[0]},
        {"row": 1, "col": 1, "x": 300.0, "y": 300.0, "z": 0.0, "yaw": 0.0, "objTypeNum": 42},
    ]
    manifest = _make_manifest(tmp_path, pak_path, 60, pool[0], placements)
    with pytest.raises(ValueError, match="RESERVED_SCATTER_OBJTYPENUMS"):
        st.cook_scatter(pak_path, manifest, tmp_path / "out.pak",
                         prop_verified=True, allow_unverified=False)


# --- round-trip prior-packet byte-identity -----------------------------------
@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(), reason="Carver5 fixture pak not present")
def test_cook_roundtrip_byte_identity_on_real_pak_copy(tmp_path):
    """Copy a real stock mission .pak, plan a small scatter, cook it, then
    verify every OTHER packet (not the object packet) is byte-identical
    pre/post, mirroring install_cliff_dressing's object2.pak verification."""
    src = CARVER5_MISSIONS / "mc2_01.pak"
    pak_copy = tmp_path / "mc2_01_copy.pak"
    shutil.copyfile(src, pak_copy)

    plan_out = tmp_path / "plan"
    r = run("plan", "mc2_01", "--missions-dir", str(tmp_path.parent), "--out", str(plan_out),
            "--count", "10", "--seed", "5")
    # plan reads from --missions-dir directly by mission name; point it at
    # the copy directly instead.
    plan_out2 = tmp_path / "plan2"
    (tmp_path / "fixture_dir").mkdir(exist_ok=True)
    fixture_pak = tmp_path / "fixture_dir" / "mc2_01.pak"
    shutil.copyfile(src, fixture_pak)
    fit_src = CARVER5_MISSIONS / "mc2_01.fit"
    if fit_src.is_file():
        shutil.copyfile(fit_src, tmp_path / "fixture_dir" / "mc2_01.fit")
    r2 = run("plan", "mc2_01", "--missions-dir", str(tmp_path / "fixture_dir"),
             "--out", str(plan_out2), "--count", "10", "--seed", "5")
    assert r2.returncode == 0, r2.stderr

    manifest_path = plan_out2 / "mc2_01.scatter_manifest.json"
    out_pak = tmp_path / "mc2_01_cooked.pak"
    stats = st.cook_scatter(fixture_pak, manifest_path, out_pak, prop_verified=False, allow_unverified=True)

    before = pak_append.read_packets(fixture_pak)
    after = pak_append.read_packets(out_pak)
    assert len(before) == len(after)
    for b, a in zip(before, after):
        if b.index == stats["object_packet_index"]:
            continue
        assert b.payload == a.payload, f"packet {b.index} changed unexpectedly"
