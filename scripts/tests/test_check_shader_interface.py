"""Planted-violation fixtures for scripts/check_shader_interface.py (one fixture per carried assertion).

A copy of the real shader inputs is built once; each case mutates it, runs one family, and restores it. CASES is
also consumed by the old-vs-new proof (the retired scripts ran on the same fixtures): a case is
(name, family, mutate(root), must_fail, old_must_fail). old_must_fail differs from must_fail only where the fold
strengthened a check (schema: a missing GLSL source/struct printed FAIL but exited 0)."""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import check_shader_interface as csi  # noqa: E402

COPIED = ["shaders", "tools/shader_offline_build/pilots.json", "tools/shader_reflect/expected", "tools/shader_schema",
          "docs/render-backend-seams/binding-slot-occupancy.json", "RenderCore/gpu_abi", "RenderCore/MaterialGpu.h",
          "GameOS/gameos/gos_vegetation.cpp", "GameOS/gameos/vk_vegetation_state.h", "GameOS/gameos/CMakeLists.txt",
          "scripts/shader_interface_rules.json", "GameOS/gameos/vulkan_vfx_island.cpp"]
SPV = "shaders/spv"
VEG = "GameOS/gameos/gos_vegetation.cpp"
VEG_H = "GameOS/gameos/vk_vegetation_state.h"
VEG_GLSL = "shaders/vulkan/include/vegetation_structs.glsl"


def build_tree(dst: Path) -> None:
    for rel in COPIED:
        src, out = ROOT / rel, dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, out)


def _rw_json(root, rel, fn):
    p = root / rel
    data = json.loads(p.read_text(encoding="utf-8"))
    fn(data)
    p.write_text(json.dumps(data, indent=2), encoding="utf-8")


def _sub(root, rel, old, new, count=1):
    p = root / rel
    text = p.read_text(encoding="utf-8")
    assert old in text, f"fixture anchor {old!r} not in {rel}"
    p.write_text(text.replace(old, new, count), encoding="utf-8")


def _first_sidecar(root, pred=lambda m: True):
    for p in sorted((root / SPV).glob("*.json")):
        if p.name.startswith("spirv_"):
            continue
        m = json.loads(p.read_text(encoding="utf-8"))
        if pred(m):
            return p.relative_to(root).as_posix(), m
    raise AssertionError("no sidecar matches the fixture predicate")


def _has_buffers(m):
    return bool(m.get("bindings", {}).get("ubos") or m.get("bindings", {}).get("ssbos"))


def _bump_offset(root):
    """Move the last member of reflected GpuMechInstance (validate_abi compares it with the generated header)."""
    def fn(m):
        for t in m["types"].values():
            if t["name"] == "GpuMechInstance" and "offset" in t["members"][-1]:
                t["members"][-1]["offset"] += 4
    _rw_json(root, f"{SPV}/mech.vert.default.json", fn)


def _second_variant(root, mutate_probe, stage_pair=False):
    """Plant a second baked variant of mech (defines X) so the cross-variant rules have something to compare."""
    def add(cfg):
        p = next(p for p in cfg["pilots"] if p["program"] == "mech")
        p["variants"].append({"name": "probe", "defines": ["X"]})
    _rw_json(root, "tools/shader_offline_build/pilots.json", add)
    for st in ("vert", "frag"):
        m = json.loads((root / SPV / f"mech.{st}.default.json").read_text(encoding="utf-8"))
        m["defines"] = ["X"]
        if st == "vert" or stage_pair:
            mutate_probe(st, m)
        (root / SPV / f"mech.{st}.probe.json").write_text(json.dumps(m, indent=2), encoding="utf-8")


def _probe_first(kind, field, value):
    def fn(st, m):
        if st != "vert":
            return
        group = m["interface" if kind in ("inputs", "outputs") else "bindings"][kind]
        if value is None:
            del group[0]
        else:
            group[0][field] = value
    return fn


def _append(root, rel, text):
    with open(root / rel, "a", encoding="utf-8") as f:
        f.write(text)


def _veg_members(root):
    _sub(root, VEG_H, "planeLen", "planeLenX")


CASES = [
    # artifacts (check-spirv-artifacts.py)
    ("a sidecar missing", "artifacts", lambda r: (r / SPV / "mech.frag.default.json").unlink(), True, True),
    ("a spv missing", "artifacts", lambda r: (r / SPV / "mech.vert.default.dbbae7d3.spv").unlink(), True, True),
    ("a source drift", "artifacts", lambda r: _append(r, "shaders/vulkan/mech_tier1.frag", "\n// drift\n"), True, True),
    ("a include drift", "artifacts", lambda r: _sub(r, "shaders/include/gpu_abi_mech_instance.gen.hglsl", "struct", "struct /*d*/"), True, True),
    ("a types missing", "artifacts", lambda r: _rw_json(r, _first_sidecar(r, _has_buffers)[0], lambda m: m.update(types={})), True, True),
    ("a abi offset moved", "artifacts", _bump_offset, True, True),
    ("a occupancy empty", "artifacts", lambda r: (r / "docs/render-backend-seams/binding-slot-occupancy.json").write_text('{"occupancy": {}}'), True, True),
    ("a occupancy missing", "artifacts", lambda r: (r / "docs/render-backend-seams/binding-slot-occupancy.json").unlink(), True, True),
    ("a index missing", "artifacts", lambda r: (r / SPV / "spirv_index.json").unlink(), True, True),
    ("a index key stale", "artifacts", lambda r: _rw_json(r, f"{SPV}/spirv_index.json", lambda d: d["records"][0].update(artifact="x.spv")), True, True),
    ("a clean", "artifacts", lambda r: None, False, False),
    # reflection (check-spirv-reflection-contract.py)
    ("r types missing", "reflection", lambda r: _rw_json(r, _first_sidecar(r, _has_buffers)[0], lambda m: m.update(types={})), True, True),
    ("r abi offset moved", "reflection", _bump_offset, True, True),
    ("r occupancy empty", "reflection", lambda r: (r / "docs/render-backend-seams/binding-slot-occupancy.json").write_text('{"occupancy": {}}'), True, True),
    ("r clean second variant", "reflection", lambda r: _second_variant(r, lambda st, m: None, True), False, False),
    ("r vert input moved", "reflection", lambda r: _second_variant(r, _probe_first("inputs", "location", 13)), True, True),
    ("r binding moved", "reflection", lambda r: _second_variant(r, _probe_first("ubos", "binding", 13)), True, True),
    ("r superset drops input", "reflection", lambda r: _second_variant(r, _probe_first("inputs", None, None)), True, True),
    ("r superset drops ubo", "reflection", lambda r: _second_variant(r, _probe_first("ubos", None, None)), True, True),
    ("r varying mislink", "reflection", lambda r: _rw_json(r, f"{SPV}/mech.frag.default.json",
                                                            lambda m: m["interface"]["inputs"][0].update(location=13)), True, True),
    # package (check-shader-package.py)
    ("p package missing", "package", lambda r: (r / SPV / "spirv_package.json").unlink(), True, True),
    ("p schema version", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json", lambda d: d.update(schema_version=99)), True, True),
    ("p pilot families", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json", lambda d: d["pilot_families"].pop()), True, True),
    ("p tool field", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json", lambda d: d["tools"].pop("spirv_cross")), True, True),
    ("p policy field", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json", lambda d: d["runtime"].pop("fatal_gate")), True, True),
    ("p excluded families", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json", lambda d: d.pop("excluded_families")), True, True),
    ("p variant matrix", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json", lambda d: d["variant_matrix"].pop()), True, True),
    ("p declared spv missing", "package", lambda r: (r / SPV / "mech.frag.default.dbbae7d3.spv").unlink(), True, True),
    ("p sidecar missing", "package", lambda r: (r / SPV / "mech.frag.default.json").unlink(), True, True),
    ("p reflection not captured", "package", lambda r: _rw_json(r, f"{SPV}/mech.vert.default.json", lambda m: m.pop("interface")), True, True),
    ("p stale hash", "package", lambda r: _rw_json(r, f"{SPV}/spirv_package.json",
                                                   lambda d: d["hashes"].update({next(iter(d["hashes"])): "0" * 64})), True, True),
    ("p index missing", "package", lambda r: (r / SPV / "spirv_index.json").unlink(), True, True),
    ("p undeclared spv", "package", lambda r: (r / SPV / "stray.spv").write_bytes(b"\x03\x02\x23\x07"), True, True),
    ("p clean", "package", lambda r: None, False, False),
    # schema (check-shader-schema.sh -> tools/shader_schema/validate.py)
    ("s golden stride", "schema", lambda r: _sub(r, "tools/shader_reflect/expected/shaders__fixtures__material_gpu_contract.frag__default.json",
                                                 '"array_stride": 32', '"array_stride": 36'), True, True),
    ("s golden missing", "schema", lambda r: (r / "tools/shader_reflect/expected/shaders__vulkan__mech_tier1.vert__default.json").unlink(), True, True),
    ("s required golden missing", "schema", lambda r: _rw_json(r, "tools/shader_schema/manifest.json",
                                                               lambda d: d["interfaces"][0]["requiredShaders"].append("shaders/none.frag")), True, True),
    ("s cpp sizeof", "schema", lambda r: _sub(r, "RenderCore/MaterialGpu.h", "sizeof(MaterialGpu) == 32", "sizeof(MaterialGpu) == 36"), True, True),
    ("s cpp offsetof", "schema", lambda r: _sub(r, "RenderCore/MaterialGpu.h", "flags)                == 16", "flags)                == 12"), True, True),
    ("s cpp header missing", "schema", lambda r: (r / "RenderCore/MaterialGpu.h").unlink(), True, True),
    ("s glsl size", "schema", lambda r: _sub(r, "shaders/include/material_gpu.hglsl", "float roughnessFactor;", "vec2 roughnessFactor;"), True, True),
    ("s glsl field order", "schema", lambda r: _sub(r, "shaders/include/material_gpu.hglsl", "uint  albedoTex;", "uint  albedoTexRenamed;"), True, True),
    ("s glsl unknown type", "schema", lambda r: _sub(r, "shaders/include/material_gpu.hglsl", "float roughnessFactor;", "mat4x3 roughnessFactor;"), True, True),
    ("s manifest empty", "schema", lambda r: (r / "tools/shader_schema/manifest.json").write_text('{"interfaces": []}'), True, True),
    ("s manifest missing", "schema", lambda r: (r / "tools/shader_schema/manifest.json").unlink(), True, True),
    # strengthened: validate.py printed FAIL here but exited 0
    ("s glsl source missing", "schema", lambda r: (r / "shaders/include/material_gpu.hglsl").unlink(), True, False),
    ("s glsl struct missing", "schema", lambda r: _sub(r, "shaders/include/material_gpu.hglsl", "struct MaterialGpu", "struct MaterialGpuX"), True, False),
    ("s clean", "schema", lambda r: None, False, False),
    # rules (check-veg-gpu-cull.py)
    ("v cpu vertex build", "rules", lambda r: _append(r, VEG, "\nstatic VfxGeometryVertex v;\n"), True, True),
    ("v cpu instance loop", "rules", lambda r: _append(r, VEG, "\n// for (size_t index = 0;\n"), True, True),
    ("v cpu instance index", "rules", lambda r: _append(r, VEG, "\n// instances[0]\n"), True, True),
    ("v cpu population copy", "rules", lambda r: _append(r, VEG, "\n// std::vector<GosVegetation::Instance> instances\n"), True, True),
    ("v no gpu run", "rules", lambda r: _sub(r, VEG, "mc2_vk_vfx_vegetation_run", "mc2_vk_vfx_vegetation_nop", 99), True, True),
    ("v cullparams members", "rules", _veg_members, True, True),
    ("v cullparams size", "rules", lambda r: _sub(r, VEG_H, "sizeof(CullParams) == 176", "sizeof(CullParams) == 192"), True, True),
    ("v glsl members", "rules", lambda r: _sub(r, VEG_GLSL, "planeLen", "planeLenX"), True, True),
    ("v include dropped", "rules", lambda r: _sub(r, "shaders/vulkan/vfx_vegetation.vert", "vegetation_structs.glsl", "vegetation_other.glsl", 99), True, True),
    ("v not baked", "rules", lambda r: _sub(r, "GameOS/gameos/CMakeLists.txt", "vegetation_compact", "vegetation_kompact", 99), True, True),
    ("v source missing", "rules", lambda r: [p.unlink() for p in (r / "shaders/vulkan").glob("vegetation_region.*")], True, True),
    ("v clean", "rules", lambda r: None, False, False),
    ("v shade selection", "rules", lambda r: _sub(r, "GameOS/gameos/vulkan_vfx_island.cpp", "depthOnly ? s->vegFrag.get() : s->vegShadeFrag.get()", "s->vegShadeFrag.get()"), True, False),
    ("v shade alpha", "rules", lambda r: _sub(r, "shaders/vulkan/vfx_vegetation.frag", "if (texel.a * alphaLift < 0.5) discard;", ""), True, False),
    ("v shade spatial", "rules", lambda r: _sub(r, "shaders/vulkan/vfx_vegetation.frag", "if (v_atlasLayer >= 4u && v_vegAux.x <= coverageRank) discard;", ""), True, False),
    ("v early depth writes", "rules", lambda r: _sub(r, "shaders/vulkan/vfx_vegetation.frag", "#ifdef VEGETATION_SHADE_EARLY_TESTS", "#if 1"), True, False),
]


@pytest.fixture(scope="module")
def clean_tree(tmp_path_factory):
    root = tmp_path_factory.mktemp("shader_iface") / "clean"
    build_tree(root)
    return root


def test_real_tree_is_clean():
    assert csi.main(["--quiet"]) == 0


@pytest.mark.parametrize("name,family,mutate,must_fail,_old", CASES, ids=[c[0] for c in CASES])
def test_planted_violation(clean_tree, tmp_path, name, family, mutate, must_fail, _old):
    root = tmp_path / "t"
    shutil.copytree(clean_tree, root)
    mutate(root)
    assert csi.main(["--root", str(root), "--families", family, "--quiet"]) == (1 if must_fail else 0), name


def test_every_family_has_a_failing_and_a_clean_case():
    for fam in csi.FAMILIES:
        assert {c[3] for c in CASES if c[1] == fam} == {True, False}, fam


def test_unknown_family_is_an_error():
    with pytest.raises(SystemExit):
        csi.main(["--families", "nope"])
