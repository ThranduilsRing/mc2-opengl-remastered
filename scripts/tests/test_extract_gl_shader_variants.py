"""Material-loader extraction must survive retiring a tessellated shader."""
import importlib.util
from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("extract_gl_variants", ROOT / "scripts/extract_gl_shader_variants.py")
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)


@pytest.mark.parametrize("condition, expected", [
    ('if (strcmp(shader, "gos_terrain") == 0) {', {"gos_terrain"}),
    ('if (strcmp(shader, "gos_terrain") == 0 || strcmp(shader, "shadow_terrain") == 0) {', {"gos_terrain", "shadow_terrain"}),
])
def test_tessellation_materials_follow_the_loader(condition, expected):
    assert extractor.material_tess_shaders(condition) == expected


def test_unknown_loader_shape_fails_instead_of_omitting_tessellation():
    with pytest.raises(extractor.ExtractError):
        extractor.material_tess_shaders("if (new_tess_policy(shader)) {")


def test_retired_generic_pool_keeps_literal_terrain_materials(monkeypatch):
    # The generic shader_list/combinations pool is gone; terrain readiness still
    # loads a literal material and must stay in the offline compilation census.
    records = []
    world = extractor.World()
    monkeypatch.setattr(extractor, "WORLD", world)
    extractor.collect_materials(world, records)
    assert records
    assert all(r["id"].startswith("gos_terrain") for r in records)


def test_retired_terrain_compute_has_no_gl_variant(monkeypatch):
    from types import SimpleNamespace
    world = extractor.World()
    monkeypatch.setattr(world, "src", lambda _: SimpleNamespace(raw="", t="void PackAndDispatch() {}"))
    records = []
    extractor.collect_terrain_lighting(world, records)
    assert records == []


@pytest.mark.parametrize("source", [
    "tl_build_terrain_lighting_program(new_path);",
    'tl_build_terrain_lighting_program("shaders/light.comp", "shared.hglsl");',
])
def test_live_terrain_loader_still_rejects_unknown_splice(monkeypatch, source):
    from types import SimpleNamespace
    world = extractor.World()
    monkeypatch.setattr(world, "src", lambda _: SimpleNamespace(raw=source, t=source))
    with pytest.raises(extractor.ExtractError):
        extractor.collect_terrain_lighting(world, [])


def test_retired_static_prop_programs_have_no_gl_variants():
    records = extractor.extract()["variants"]
    assert not any("static_prop" in stage for record in records for stage in record["stages"].values())
