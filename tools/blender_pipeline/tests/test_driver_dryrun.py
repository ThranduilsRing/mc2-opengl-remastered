"""Driver plan / dry-run / GLB-patch / runner tests (no Blender required)."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import batch_driver as bd  # noqa: E402
import blender_runner as br  # noqa: E402


@pytest.fixture
def manifest_dir(tmp_path):
    (tmp_path / "quonset.ase").write_text("*GEOMOBJECT {\n}\n")
    (tmp_path / "quonset.ini").write_text(
        'FITini\n\n[TGLData]\nst FileName0="Quonset"\n\nFITend\n')
    (tmp_path / "maple1.glb").write_bytes(b"glTF")
    manifest = {
        "schema_version": "1.0",
        "jobs": [
            {
                "name": "quonset_hd",
                "recipe": "upscale_mesh",
                "asset_class": "building",
                "source": {"ase": "quonset.ase", "ini": "quonset.ini"},
                "params": {"subdiv": 2, "crease_angle": 35.0},
                "out": {"kind": "tgl_ini", "dir": "out",
                        "source_name": "QuonsetHD"},
            },
            {
                "name": "maple1_lods",
                "recipe": "decimate_lods",
                "asset_class": "tree",
                "source": {"glb": "maple1.glb"},
                "params": {"ratios": [0.5, 0.2], "distances": [600, 1200]},
                "out": {"kind": "models_json", "dir": "out",
                        "override_class": "tree", "appearance_name": "maple1"},
            },
        ],
    }
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))
    return tmp_path


def ctx_for(tmp_path):
    return {"base_dir": tmp_path, "work_dir": tmp_path / "_work",
            "deploy_tgl_dir": None}


def test_plan_upscale_stages(manifest_dir):
    manifest = bd.load_manifest(manifest_dir / "manifest.json")
    stages = bd.plan_job(manifest["jobs"][0], ctx_for(manifest_dir))
    kinds = [s["stage"] for s in stages]
    assert kinds == ["extract", "transform", "patch_textures", "inject"]

    extract = stages[0]
    assert "--ase" in extract["cmd"]
    assert str(manifest_dir / "quonset.ase") in extract["cmd"]
    assert "--asset-class" in extract["cmd"]

    transform = stages[1]
    assert transform["script"].endswith("upscale_mesh.py")
    assert transform["args"]["subdiv"] == 2
    assert transform["args"]["crease-angle"] == 35.0


def test_plan_lods_stages(manifest_dir):
    manifest = bd.load_manifest(manifest_dir / "manifest.json")
    stages = bd.plan_job(manifest["jobs"][1], ctx_for(manifest_dir))
    kinds = [s["stage"] for s in stages]
    assert kinds == ["extract", "transform", "inject"]  # no texture patch
    assert stages[0]["copy"][0] == str(manifest_dir / "maple1.glb")
    assert stages[1]["args"]["ratios"] == "0.5,0.2"
    assert stages[1]["args"]["name"] == "maple1"


def test_dry_run_exits_zero_and_prints_plan(manifest_dir, capsys):
    rc = bd.main([str(manifest_dir / "manifest.json"), "--dry-run"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "DRY RUN" in out
    assert "quonset_hd" in out and "maple1_lods" in out
    assert "upscale_mesh.py" in out


def test_dry_run_invalid_manifest_exits_two(manifest_dir, capsys):
    bad = json.loads((manifest_dir / "manifest.json").read_text())
    bad["jobs"][0]["recipe"] = "nope"
    (manifest_dir / "bad.json").write_text(json.dumps(bad))
    rc = bd.main([str(manifest_dir / "bad.json"), "--dry-run"])
    assert rc == 2
    assert "MANIFEST INVALID" in capsys.readouterr().err


def test_only_filter_unknown_job(manifest_dir, capsys):
    rc = bd.main([str(manifest_dir / "manifest.json"), "--dry-run",
                  "--only", "ghost"])
    assert rc == 2


# ---------------------------------------------------------------------------
# GLB read/write + texture carry-over
# ---------------------------------------------------------------------------

def minimal_gltf(materials, images=None, textures=None):
    g = {"asset": {"version": "2.0"}, "materials": materials}
    if images:
        g["images"] = images
    if textures:
        g["textures"] = textures
    return g


def test_glb_roundtrip(tmp_path):
    gltf = minimal_gltf([{"name": "Quonset"}])
    p = tmp_path / "t.glb"
    bd.write_glb(gltf, b"\x01\x02\x03", p)
    back, binary = bd.read_glb(p)
    assert back["materials"][0]["name"] == "Quonset"
    assert binary.rstrip(b"\x00") == b"\x01\x02\x03"


def test_carry_textures_patches_by_material_name():
    src = minimal_gltf(
        [{"name": "Quonset",
          "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        images=[{"uri": "A_Quonset.tga"}], textures=[{"source": 0}])
    dst = minimal_gltf([{"name": "Quonset.001",
                         "pbrMetallicRoughness": {"metallicFactor": 0.0}}])
    patched = bd.carry_textures(src, dst)
    assert patched == 1
    assert dst["images"] == [{"uri": "A_Quonset.tga"}]
    assert dst["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] == {
        "index": 0}


def test_carry_textures_does_not_clobber_existing():
    src = minimal_gltf(
        [{"name": "M", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        images=[{"uri": "old.tga"}], textures=[{"source": 0}])
    dst = minimal_gltf(
        [{"name": "M", "pbrMetallicRoughness": {"baseColorTexture": {"index": 5}}}])
    assert bd.carry_textures(src, dst) == 0
    assert dst["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["index"] == 5


def test_carry_textures_skips_embedded_images():
    src = minimal_gltf(
        [{"name": "M", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        images=[{"bufferView": 0, "mimeType": "image/png"}], textures=[{"source": 0}])
    dst = minimal_gltf([{"name": "M"}])
    assert bd.carry_textures(src, dst) == 0


# ---------------------------------------------------------------------------
# blender_runner
# ---------------------------------------------------------------------------

def test_build_blender_cmd_arg_forms():
    cmd = br.build_blender_cmd("blender.exe", __file__,
                               {"in": "a.glb", "subdiv": 2,
                                "shade-smooth": True, "skip": None, "off": False})
    assert cmd[:3] == ["blender.exe", "--background", "--factory-startup"]
    assert "--in=a.glb" in cmd and "--subdiv=2" in cmd
    assert "--shade-smooth" in cmd
    assert not any(c.startswith("--skip") or c.startswith("--off") for c in cmd)
    assert cmd.index("--") < cmd.index("--in=a.glb")


def test_find_blender_env_override(tmp_path, monkeypatch):
    fake = tmp_path / "blender.exe"
    fake.write_bytes(b"")
    monkeypatch.setenv("BLENDER_EXECUTABLE", str(fake))
    assert br.find_blender_exe() == fake


def test_find_blender_explicit_beats_env(tmp_path, monkeypatch):
    fake_env = tmp_path / "env.exe"
    fake_env.write_bytes(b"")
    fake_cli = tmp_path / "cli.exe"
    fake_cli.write_bytes(b"")
    monkeypatch.setenv("BLENDER_EXECUTABLE", str(fake_env))
    assert br.find_blender_exe(explicit=str(fake_cli)) == fake_cli


def test_find_blender_bad_env_raises(monkeypatch):
    monkeypatch.setenv("BLENDER_EXECUTABLE", r"Z:\does\not\exist.exe")
    with pytest.raises(RuntimeError, match="BLENDER_EXECUTABLE"):
        br.find_blender_exe()


# ---------------------------------------------------------------------------
# recipe arg parsing (importable outside Blender)
# ---------------------------------------------------------------------------

def test_recipe_script_args():
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recipes"))
    import _recipe_util as ru
    argv = ["blender", "-b", "--python", "x.py", "--",
            "--in=a.glb", "--subdiv=2", "--shade-smooth"]
    args = ru.script_args(argv)
    assert args == {"in": "a.glb", "subdiv": "2", "shade-smooth": True}
    assert ru.arg(args, "subdiv", 1, int) == 2
    assert ru.arg(args, "shade-smooth", True, bool) is True
    assert ru.arg(args, "missing", 7, int) == 7
    assert ru.float_list("0.5,0.2") == [0.5, 0.2]
