"""asset_edit.py tests — workdir layout, mtime edit detection, inject plan.
No Blender/GUI required."""
import json
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import asset_edit as ae  # noqa: E402
import batch_driver as bd  # noqa: E402


# ---------------------------------------------------------------------------
# Stock asset discovery
# ---------------------------------------------------------------------------

@pytest.fixture
def srcdata(tmp_path):
    tgl = tmp_path / "srcdata" / "tgl"
    tgl.mkdir(parents=True)
    (tgl / "quonset.ase").write_text("*GEOMOBJECT {\n}\n")
    (tgl / "quonset.ini").write_text(
        'FITini\n\n[TGLData]\nst FileName0="Quonset"\n\nFITend\n')
    return tmp_path / "srcdata"


def test_find_stock_asset_explicit_root(srcdata):
    ase, ini = ae.find_stock_asset("quonset", srcdata=srcdata)
    assert ase == srcdata / "tgl" / "quonset.ase"
    assert ini == srcdata / "tgl" / "quonset.ini"


def test_find_stock_asset_env_root(srcdata, monkeypatch):
    monkeypatch.setenv("MC2_SRCDATA", str(srcdata))
    ase, _ = ae.find_stock_asset("quonset")
    assert ase == srcdata / "tgl" / "quonset.ase"


def test_find_stock_asset_missing_lists_probes(srcdata):
    with pytest.raises(RuntimeError, match="ghost.*Probed"):
        ae.find_stock_asset("ghost", srcdata=srcdata)


def test_find_stock_asset_missing_ini(srcdata):
    (srcdata / "tgl" / "quonset.ini").unlink()
    with pytest.raises(RuntimeError, match="FITini sibling"):
        ae.find_stock_asset("quonset", srcdata=srcdata)


# ---------------------------------------------------------------------------
# Workdir layout + edit detection
# ---------------------------------------------------------------------------

def test_workdir_layout(tmp_path):
    paths = ae.paths_for("quonset", tmp_path / "work")
    assert paths["work"] == tmp_path / "work" / "quonset"
    assert paths["glb"].name == "quonset.glb"
    assert paths["stock_glb"].name == "quonset.stock.glb"
    assert paths["sidecar"].name == "quonset.mcasset.json"
    assert paths["session"].name == "session.json"
    assert all(p.parent == paths["work"] for k, p in paths.items()
               if k != "work")


def test_session_roundtrip_and_glb_states(tmp_path):
    paths = ae.paths_for("quonset", tmp_path / "work")
    paths["work"].mkdir(parents=True)
    paths["glb"].write_bytes(b"glTF-original")

    session = {"asset": "quonset"}
    ae.snapshot_glb(session, paths["glb"])
    ae.save_session(paths, session)
    back = ae.load_session(paths)
    assert back["glb_mtime_ns"] == session["glb_mtime_ns"]

    assert ae.glb_state(back, paths["glb"]) == "unchanged"

    # Simulate the Blender re-export: same size, newer mtime.
    st = paths["glb"].stat()
    os.utime(paths["glb"], ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))
    assert ae.glb_state(back, paths["glb"]) == "edited"

    paths["glb"].unlink()
    assert ae.glb_state(back, paths["glb"]) == "missing"


def test_load_session_never_opened(tmp_path):
    assert ae.load_session(ae.paths_for("ghost", tmp_path)) is None


# ---------------------------------------------------------------------------
# Launch cmd builders
# ---------------------------------------------------------------------------

def test_gui_launch_cmd_is_not_background():
    cmd = ae.build_gui_launch_cmd("blender.exe", "w/q.glb", "quonset")
    assert cmd[0] == "blender.exe"
    assert "--background" not in cmd and "--factory-startup" not in cmd
    assert "--python" in cmd
    assert cmd[cmd.index("--python") + 1].endswith("asset_edit_open.py")
    assert "--glb=w/q.glb" in cmd and "--asset=quonset" in cmd


def test_socket_import_code_shape(tmp_path):
    glb = tmp_path / "q.glb"  # exercises backslash escaping on Windows
    code = ae.build_socket_import_code(glb, "quonset")
    assert "bpy.ops.import_scene.gltf" in code
    assert repr(str(glb)) in code
    assert "custom_normal" in code
    assert "export quonset" in code
    compile(code, "<socket>", "exec")  # must be valid python


# ---------------------------------------------------------------------------
# Export plan (inject)
# ---------------------------------------------------------------------------

@pytest.fixture
def opened(tmp_path):
    """A fake opened workdir: session + work GLB + stock GLB + sidecar."""
    srcdir = tmp_path / "srcdata" / "tgl"
    srcdir.mkdir(parents=True)
    ini = srcdir / "quonset.ini"
    ini.write_text('FITini\n\n[TGLData]\nst FileName0="Quonset"\n\nFITend\n')

    paths = ae.paths_for("quonset", tmp_path / "work")
    paths["work"].mkdir(parents=True)
    paths["glb"].write_bytes(b"glTF-edited")
    paths["stock_glb"].write_bytes(b"glTF-stock")
    paths["sidecar"].write_text("{}")
    session = {"asset": "quonset", "ase": str(srcdir / "quonset.ase"),
               "ini": str(ini)}
    return paths, session, tmp_path / "out" / "quonset"


def test_plan_export_tgl_ini(opened):
    paths, session, out_dir = opened
    actions = ae.plan_export("quonset", session, paths, out_dir, "tgl_ini",
                             "QuonsetEdit")
    kinds = [a["action"] for a in actions]
    assert kinds == ["copy", "write_ini", "copy"]
    assert actions[0]["dst"].endswith("QuonsetEdit.glb")
    assert actions[1]["dst"].endswith("quonset.ini")
    assert actions[1]["source_name"] == "QuonsetEdit"
    assert actions[2]["dst"].endswith("QuonsetEdit.mcasset.json")

    written = ae.run_export_plan(actions)
    assert len(written) == 3
    ini_text = (out_dir / "quonset.ini").read_text()
    assert '[Import]' in ini_text
    assert 'st Source = "QuonsetEdit"' in ini_text
    assert (out_dir / "QuonsetEdit.glb").read_bytes() == b"glTF-edited"


def test_plan_export_tgl_ini_deploy_passthrough(opened, tmp_path):
    paths, session, out_dir = opened
    deploy = tmp_path / "deploy" / "data" / "tgl"
    actions = ae.plan_export("quonset", session, paths, out_dir, "tgl_ini",
                             "QuonsetEdit", deploy_tgl_dir=deploy)
    deploy_dsts = [a["dst"] for a in actions
                   if a["action"] == "copy" and str(deploy) in a["dst"]]
    assert len(deploy_dsts) == 3  # glb + ini + sidecar mirrored

    ae.run_export_plan(actions)
    assert (deploy / "QuonsetEdit.glb").is_file()
    assert 'st Source = "QuonsetEdit"' in (deploy / "quonset.ini").read_text()


def test_plan_export_glb_only(opened):
    paths, session, out_dir = opened
    actions = ae.plan_export("quonset", session, paths, out_dir, "glb_only",
                             "QuonsetEdit")
    assert [a["action"] for a in actions] == ["copy"]
    assert actions[0]["dst"].endswith("QuonsetEdit.glb")


def test_plan_export_models_json_entry_shape(opened):
    paths, session, out_dir = opened
    actions = ae.plan_export("quonset", session, paths, out_dir,
                             "models_json", "QuonsetEdit",
                             override_class="staticprop")
    assert [a["action"] for a in actions] == ["copy", "models_json"]
    assert actions[0]["dst"].endswith("quonset_lod0.glb")
    entry = actions[1]["entry"]
    assert entry["replaces"] == "staticprop:quonset"
    assert entry["renderOnly"] is True and entry["fallback"] == "stock"
    assert entry["lods"][0]["distance"] == 0
    assert entry["lods"][0]["source"].endswith("quonset_lod0.glb")

    ae.run_export_plan(actions)
    manifest = json.loads((out_dir / "models.json").read_text())
    assert manifest[0]["appearanceName"] == "quonset"
    # Re-export merges (no stale duplicate) — engine rule: first key wins.
    ae.run_export_plan(actions)
    manifest = json.loads((out_dir / "models.json").read_text())
    assert len(manifest) == 1


# ---------------------------------------------------------------------------
# Post-export GLB validation
# ---------------------------------------------------------------------------

def _gltf_with_mesh(*, texcoord=True, indices_count=6, pos_count=None,
                    with_texture=False, with_image=True):
    attrs = {"POSITION": 0}
    if texcoord:
        attrs["TEXCOORD_0"] = 1
    accessors = [{"count": pos_count if pos_count is not None else 4}]
    prim = {"attributes": attrs}
    if indices_count is not None:
        accessors.append({"count": indices_count})
        prim["indices"] = 1
    gltf = {"asset": {"version": "2.0"},
            "meshes": [{"primitives": [prim]}],
            "accessors": accessors}
    if with_texture:
        gltf["materials"] = [{"pbrMetallicRoughness":
                              {"baseColorTexture": {"index": 0}}}]
        if with_image:
            gltf["images"] = [{"uri": "A_Quonset.tga"}]
    return gltf


def test_validate_exported_glb_ok():
    gltf = _gltf_with_mesh(with_texture=True)
    assert ae.validate_exported_glb(gltf) is None


def test_validate_exported_glb_no_meshes_is_not_flagged():
    # A GLB fragment with no mesh data has nothing to validate geometrically
    # (distinct from a round-trip that lost UVs on existing geometry).
    assert ae.validate_exported_glb({"asset": {"version": "2.0"}}) is None


def test_validate_exported_glb_missing_texcoord0():
    gltf = _gltf_with_mesh(texcoord=False)
    with pytest.raises(RuntimeError, match="TEXCOORD_0"):
        ae.validate_exported_glb(gltf)


def test_validate_exported_glb_zero_triangles():
    gltf = _gltf_with_mesh(indices_count=0)
    with pytest.raises(RuntimeError, match="triangle count is 0"):
        ae.validate_exported_glb(gltf)


def test_validate_exported_glb_absurd_triangles():
    gltf = _gltf_with_mesh(indices_count=ae.MAX_SANE_TRI_COUNT * 3 + 3)
    with pytest.raises(RuntimeError, match="sanity ceiling"):
        ae.validate_exported_glb(gltf)


def test_validate_exported_glb_lost_texture_uri():
    gltf = _gltf_with_mesh(with_texture=True, with_image=False)
    with pytest.raises(RuntimeError, match="texture carry-over likely failed"):
        ae.validate_exported_glb(gltf)


def test_export_full_flow_runs_validator_and_fails_loudly(opened, tmp_path):
    """A GLB that lost TEXCOORD_0 (the custom_normal bug) must fail export
    loudly rather than silently writing a broken asset to out/."""
    paths, session, _ = opened
    stock = {"asset": {"version": "2.0"}, "materials": [], "images": []}
    broken = _gltf_with_mesh(texcoord=False)
    bd.write_glb(stock, None, paths["stock_glb"])
    bd.write_glb(broken, b"\x00\x00\x00\x00", paths["glb"])
    ae.snapshot_glb(session, paths["glb"])
    session["extracted_at"] = "test"
    ae.save_session(paths, session)
    st = paths["glb"].stat()
    os.utime(paths["glb"], ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))

    work_root = str(paths["work"].parent)
    out_root = str(tmp_path / "outcli_broken")
    rc = ae.main(["--work-root", work_root, "--out-root", out_root,
                  "export", "quonset"])
    assert rc == 1
    assert not (Path(out_root) / "quonset").is_dir()


# ---------------------------------------------------------------------------
# CLI-level behavior (no Blender)
# ---------------------------------------------------------------------------

def test_status_never_opened(tmp_path, capsys):
    rc = ae.main(["--work-root", str(tmp_path / "work"),
                  "--out-root", str(tmp_path / "out"),
                  "status", "ghost"])
    assert rc == 0
    assert "not opened" in capsys.readouterr().out


def test_export_never_opened_exits_two(tmp_path, capsys):
    rc = ae.main(["--work-root", str(tmp_path / "work"),
                  "--out-root", str(tmp_path / "out"),
                  "export", "ghost"])
    assert rc == 2
    assert "never opened" in capsys.readouterr().err


def test_export_full_flow_with_texture_carry(opened, tmp_path, capsys):
    """End-to-end export against real minimal GLBs: the edited GLB lost its
    baseColor texture; export re-attaches the stock URI and injects tgl_ini."""
    paths, session, _ = opened

    stock = {"asset": {"version": "2.0"},
             "materials": [{"name": "Quonset", "pbrMetallicRoughness":
                            {"baseColorTexture": {"index": 0}}}],
             "images": [{"uri": "A_Quonset.tga"}], "textures": [{"source": 0}]}
    edited = {"asset": {"version": "2.0"},
              "materials": [{"name": "Quonset.001",
                             "pbrMetallicRoughness": {"metallicFactor": 0.0}}]}
    bd.write_glb(stock, None, paths["stock_glb"])
    bd.write_glb(edited, b"\x00\x00\x00\x00", paths["glb"])
    ae.snapshot_glb(session, paths["glb"])
    session["extracted_at"] = "test"
    ae.save_session(paths, session)
    # Newer mtime -> "edited".
    st = paths["glb"].stat()
    os.utime(paths["glb"], ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))

    work_root = str(paths["work"].parent)
    out_root = str(tmp_path / "outcli")
    rc = ae.main(["--work-root", work_root, "--out-root", out_root,
                  "export", "quonset"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "1 material(s) patched" in out

    out_glb = Path(out_root) / "quonset" / "QuonsetEdit.glb"
    gltf, _ = bd.read_glb(out_glb)
    assert gltf["images"] == [{"uri": "A_Quonset.tga"}]
    assert 'st Source = "QuonsetEdit"' in (
        Path(out_root) / "quonset" / "quonset.ini").read_text()

    # status now reports the export dir + unchanged (export snapshot re-armed).
    rc = ae.main(["--work-root", work_root, "--out-root", out_root,
                  "status", "quonset"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "QuonsetEdit.glb" in out
