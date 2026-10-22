import struct
import re
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import material_map as mm


def mapping():
    return {"version": 1, "id": "test", "props": [{"prop": "quonset2", "slots": [{
        "slot": 0, "stockAlbedo": "A_Quonset.tga", "reviewed": True,
        "class": "rust", "normal": "normal.png", "license": "CC0", "provider": "ambientCG"}]}]}


@pytest.mark.parametrize("change", [
    {"reviewed": False}, {"class": "color_metal"}, {"normalStrength": float("nan")},
    {"uvScale": [0, 1]}, {"uvScale": [1]}, {"metallic": 2},
    {"class": "dielectric", "metallic": 0.4}, {"license": "unknown"},
    {"provider": "unverified"}, {"rotation": 45}, {"slot": -1}, {"slot": True},
])
def test_unreviewed_or_malformed_mapping_rejected(change):
    data = mapping()
    data["props"][0]["slots"][0].update(change)
    with pytest.raises(ValueError):
        mm.validate_mapping(data)


def test_mask_preserves_exclusions_and_orm_channels(tmp_path):
    normal = tmp_path / "n.png"; rough = tmp_path / "r.png"; metal = tmp_path / "m.png"; mask = tmp_path / "mask.png"
    Image.new("RGB", (4, 4), (218, 128, 218)).save(normal)
    Image.new("L", (4, 4), 80).save(rough)
    Image.new("L", (4, 4), 200).save(metal)
    Image.fromarray(np.array([[255, 255, 0, 0]]*4, dtype=np.uint8)).save(mask)
    n, orm = mm.cook_detail({"rotation": 90}, 4, {"normal": normal, "roughnessMap": rough, "metallicMap": metal, "mask": mask})
    assert orm.getpixel((0, 0)) == (255, 80, 200, 255)  # absent AO neutral, not displacement
    assert orm.getpixel((3, 0)) == (255, 255, 0, 0)  # excluded means no new shading/specular
    assert n.getpixel((3, 0)) == (128, 128, 255)
    assert n.getpixel((0, 0))[1] > 200  # normal components rotate with the image


def test_stock_geometry_and_albedo_are_reproducible(tmp_path):
    # Use the real stock extractor through a small ASE fixture from this suite.
    # The stock repository tests expose synthetic ASE through test_ase_submaterial.
    from test_ase_submaterial import SINGLE_MAT_ASE
    ase_file = tmp_path / "quonset2.ase"
    ase_file.write_text(SINGLE_MAT_ASE, encoding="utf-8")
    gltf, binary, stats = mm.stock_glb(ase_file)
    assert stats["triangle_count"] > 0
    assert all(not node.get("children") for node in gltf["nodes"])
    for mesh in gltf["meshes"]:
        for prim in mesh["primitives"]:
            acc = gltf["accessors"][prim["attributes"]["POSITION"]]
            assert acc["count"] == 3
            view = gltf["bufferViews"][acc["bufferView"]]
            points = [struct.unpack_from("<3f", binary, view["byteOffset"] + i*12) for i in range(3)]
            assert points == [(0, 0, 0), (1, 0, 0), (0, 0, 1)]
            tex = gltf["accessors"][prim["attributes"]["TEXCOORD_0"]]
            texview = gltf["bufferViews"][tex["bufferView"]]
            uv = [struct.unpack_from("<2f", binary, texview["byteOffset"] + i*8) for i in range(3)]
            # Assimp glTF2Importer.cpp flips Y on import; our toMC2V flips again.
            # Their net identity must retain the converter's stock TG 1-V.
            assert uv == [(0, 1), (1, 1), (0, 0)]


def test_archive_leaves_source_untouched(tmp_path):
    source = tmp_path / "downloads" / "normal.png"; source.parent.mkdir()
    Image.new("RGB", (2, 2), (128, 128, 255)).save(source)
    before = source.read_bytes()
    report = []
    archived = mm.archive_source(source, tmp_path / "archive", report)
    assert archived.read_bytes() == before == source.read_bytes()
    assert report[0]["sha256"] == mm.digest(source)


def test_uvless_stock_helpers_keep_zero_uv_instead_of_sampling_roof(tmp_path):
    from test_ase_submaterial import SINGLE_MAT_ASE
    text = re.sub(r'\*MESH_TVERTLIST\s*\{[^}]*\}', '', SINGLE_MAT_ASE)
    text = re.sub(r'\*MESH_TFACELIST\s*\{[^}]*\}', '', text)
    file = tmp_path / "helper.ase"; file.write_text(text)
    g, raw, _ = mm.stock_glb(file)
    for mesh in g["meshes"]:
        acc = g["accessors"][mesh["primitives"][0]["attributes"]["TEXCOORD_0"]]
        view = g["bufferViews"][acc["bufferView"]]
        uv = [struct.unpack_from("<2f", raw, view["byteOffset"] + i*8) for i in range(acc["count"])]
        assert uv == [(0, 0)] * acc["count"]  # TG's explicit no-TFACE default


def test_window_overlay_shares_albedo_but_has_no_authored_material():
    # A real failure: the window overlay's constant UV lies in the roof mask.
    # Separate slots must preserve the shared colour source and leave body on 0.
    g = {"materials": [{"name": "roof", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
         "meshes": [{"name": "body", "primitives": [{"material": 0}]},
                    {"name": "LitWin_Quonset2", "primitives": [{"material": 0}]}]}
    mm.exclude_nodes(g, ["LitWin_Quonset2"])
    assert g["meshes"][0]["primitives"][0]["material"] == 0
    assert g["meshes"][1]["primitives"][0]["material"] == 1
    assert g["materials"][0]["pbrMetallicRoughness"] == g["materials"][1]["pbrMetallicRoughness"]
    with pytest.raises(ValueError, match="unknown excluded nodes"):
        mm.exclude_nodes(g, ["unreviewed_typo"])


def test_stock_hot_color_tags_survive_extract(tmp_path):
    from test_ase_submaterial import SINGLE_MAT_ASE
    colors = """
*MESH_VERTCOL 0 1.0 0.0 1.0
*MESH_VERTCOL 1 1.0 1.0 0.0
*MESH_VERTCOL 2 0.0 1.0 0.0
*MESH_CFACE 0 0 1 2
"""
    file = tmp_path / "tags.ase"
    file.write_text(SINGLE_MAT_ASE.replace("*MESH_NUMTVFACES", colors + "*MESH_NUMTVFACES"), encoding="utf-8")
    g, raw, _ = mm.stock_glb(file)
    prim = g["meshes"][0]["primitives"][0]
    acc = g["accessors"][prim["attributes"]["COLOR_0"]]
    view = g["bufferViews"][acc["bufferView"]]
    assert list(raw[view["byteOffset"]:view["byteOffset"]+12]) == [255,0,255,255, 255,255,0,255, 0,255,0,255]


def test_bundle_keeps_albedo_and_same_image_material_slots(tmp_path, monkeypatch):
    from test_ase_submaterial import SINGLE_MAT_ASE
    src = tmp_path / "src"; src.mkdir()
    (src / "quonset2.ase").write_text(SINGLE_MAT_ASE, encoding="utf-8")
    (src / "quonset2.ini").write_text("[TGLData]\nFileName=quonset2\n", encoding="utf-8")
    textures = tmp_path / "textures"; textures.mkdir()
    albedo = textures / "plain.tga"
    Image.new("RGBA", (8, 8), (100, 50, 20, 180)).save(albedo)
    normal = tmp_path / "normal.png"
    Image.new("RGB", (8, 8), (128, 128, 255)).save(normal)
    data = mapping(); row = data["props"][0]["slots"][0]
    row.update(stockAlbedo="plain.tga", normal=str(normal))
    row2 = dict(row, slot=1, normalStrength=0.2)
    data["props"][0]["slots"].append(row2)
    original_extract = mm.stock_glb
    def two_materials(path):
        import copy
        g, raw, stats = original_extract(path)
        g["materials"].append(copy.deepcopy(g["materials"][0]))
        g["meshes"][0]["primitives"].append(dict(g["meshes"][0]["primitives"][0], material=1))
        return g, raw, stats
    monkeypatch.setattr(mm, "stock_glb", two_materials)
    original = albedo.read_bytes()
    out = tmp_path / "bundle"
    mm.build(data, out, tmp_path / "archive", src, textures, size=128)
    private = next((out / "data/tgl/128").glob("*.tga"))
    assert private.read_bytes() == original == albedo.read_bytes()
    copies = list((out / "data/tgl/128").glob("*.tga"))
    assert len(copies) == 2 and all(p.read_bytes() == original for p in copies)
    import json
    record = json.loads((out / "data/model_overrides/models.json").read_text())["overrides"][0]
    assert record["materials"]["slots"][0]["normal"].endswith("_normal.tga")
    normal_file = out / "data/model_overrides" / record["materials"]["slots"][0]["normal"]
    assert normal_file.read_bytes()[16] == 32  # native linear RGBA8 map contract
    assert record["renderOnly"] and record["fallback"] == "stock"
    assert len(record["materials"]["slots"]) == 2
    assert record["materials"]["slots"][0].get("normalStrength", 1) != record["materials"]["slots"][1].get("normalStrength", 1)
    first = (out / "data/model_overrides/models/quonset2.glb").read_bytes()
    mm.build(data, out, tmp_path / "archive", src, textures, size=128)
    assert (out / "data/model_overrides/models/quonset2.glb").read_bytes() == first
