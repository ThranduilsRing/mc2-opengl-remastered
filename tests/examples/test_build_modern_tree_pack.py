# tests/examples/test_build_modern_tree_pack.py
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "tools" / "examples"))
from build_modern_tree_pack import (
    ASSET_FAMILIES,
    build_mapping_table,
    build_mod_json,
    build_models_json,
)


def test_asset_families_structure():
    for fam in ASSET_FAMILIES:
        assert "name" in fam
        assert "replaces" in fam
        assert "class" in fam
        assert "lods" in fam
        assert len(fam["lods"]) >= 1
        for lod in fam["lods"]:
            assert "filename" in lod
            assert "distance_m" in lod


def test_replaces_format():
    for fam in ASSET_FAMILIES:
        parts = fam["replaces"].split(":")
        assert len(parts) == 2, f"replaces must be 'class:appearance', got: {fam['replaces']}"
        assert parts[0] == fam["class"]
        assert parts[1] == parts[1].lower()


def test_build_mapping_table_returns_one_row_per_lod():
    rows = build_mapping_table(ASSET_FAMILIES)
    expected = sum(len(f["lods"]) for f in ASSET_FAMILIES)
    assert len(rows) == expected
    for row in rows:
        assert row["status"] == "pending"
        assert "source_glb" in row
        assert "target_stock_appearance" in row
        assert "class" in row
        assert "lod_index" in row


def test_build_mod_json():
    result = build_mod_json("modern-tree-pack-v1", "Modern Tree Pack v1", "1.0.0")
    assert result["schema"] == "mc2-mod/1"
    assert result["id"] == "modern-tree-pack-v1"
    assert result["name"] == "Modern Tree Pack v1"
    assert result["version"] == "1.0.0"
    assert isinstance(result["dependencies"], list)


def test_build_models_json_entry_structure():
    family = {
        "name": "maple",
        "class": "tree",
        "replaces": "tree:maple1",
        "lods": [
            {"filename": "maple1.glb", "distance_m": 0},
            {"filename": "maple2.glb", "distance_m": 600},
        ],
    }
    cooked_glb_paths = {
        "maple1.glb": "data/model_overrides/cooked/maple1_lod0.glb",
        "maple2.glb": "data/model_overrides/cooked/maple1_lod1.glb",
    }
    # Engine schema {"overrides":[...]} since 7bc81a867 (2026-06-12): the old
    # bare list with appearanceName/lods[].distance+source was silently dropped
    # by mclib/model_override_registry.cpp parseManifest. LOD0 is the top-level
    # `source`; lods[] carries only LOD1+ with an integer ascending `lod`.
    entries = build_models_json([family], cooked_glb_paths)["overrides"]
    assert len(entries) == 1
    e = entries[0]
    assert e["type"] == "model"
    assert e["class"] == "tree"
    assert e["replaces"] == "tree:maple1"
    assert e["scale"] == 1.0
    assert e["source"] == "data/model_overrides/cooked/maple1_lod0.glb"
    assert len(e["lods"]) == 1
    assert e["lods"][0]["lod"] == 1
    assert e["lods"][0]["distance"] == 600
    assert e["lods"][0]["source"] == "data/model_overrides/cooked/maple1_lod1.glb"


def test_family_run_does_not_clobber_full_models_json():
    """
    build_models_json called with a subset of families must not
    include entries for families not passed in.
    """
    maple_only = [f for f in ASSET_FAMILIES if f["name"] == "maple"]
    entries = build_models_json(maple_only, {})["overrides"]  # engine schema (7bc81a867)
    assert len(entries) == 1
    assert entries[0]["replaces"] == "tree:maple1"
