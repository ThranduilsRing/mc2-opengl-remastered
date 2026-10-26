"""Fresh isolated installs must keep every deploy destination private."""
import importlib.util
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import deploy_payload as dp
import kit_payload


def test_fresh_install_covers_deploy_payload(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location(
        "isolated_deploy", ROOT / "scripts/make-isolated-deploy.py")
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    guard = iso._load_guard()
    # Enumerate real game AND editor destinations, without building/cooking.
    monkeypatch.setattr(dp, "ensure_concrete_detail", lambda root: None)
    rels = set()
    for exe in ("mc2.exe", dp.EDITOR_EXE):
        rels.update(rel for _, rel, _ in dp.enumerate_payload(
            str(ROOT), str(tmp_path / "build"), exe, "mc2.pdb",
            require_build=False))
    manifest = json.loads((ROOT / kit_payload.MANIFEST).read_text())
    rels.update((kit_payload.KIT_REL / row["path"]).as_posix()
                for row in manifest["files"])
    # Outputs written after payload copying, including cooked sidecars/receipts.
    rels.update((".deployed_manifest.csv", "data/textures/engine_normals.cookver",
                 "data/textures/mat0_normal.ktx2", "data/tgl/128/a.tga.cookstamp",
                 "cache/content-model/industrial-cook.json",
                 "cache/content-model/model-123.mci"))
    assert rels
    assert not [rel for rel in rels if guard.classify(rel) != "mutated"]
    assert guard.classify("DATA\\TGL\\128\\A.TGA.COOKSTAMP") == "mutated"
    src, dst = tmp_path / "source", tmp_path / "install"
    for rel in rels | {"data/terrain/stock.pak", "textures.fst", "data/tgl/128/stock.tga",
                       "mods/custom/data/model.glb"}:
        path = src / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"stock")
    links, copies, _, _ = iso.plan(src, guard)
    iso.build(src, dst, links, copies)
    assert guard.run_check(str(dst), payload_rels=sorted(rels))[0] == "OK"
    assert guard.run_check(str(dst), deep=True)[0] == "OK"
    assert set(guard.iter_mutated_relpaths(dst)) == set(guard.iter_mutated_relpaths(dst, deep=True))
    assert os.stat(src / "data/terrain/stock.pak").st_ino == os.stat(dst / "data/terrain/stock.pak").st_ino
    assert os.stat(src / "data/tgl/128/stock.tga").st_ino == os.stat(dst / "data/tgl/128/stock.tga").st_ino
    assert os.stat(src / "mods/custom/data/model.glb").st_ino == os.stat(dst / "mods/custom/data/model.glb").st_ino
    victim = next(iter(sorted(rels)))
    (dst / victim).write_bytes(b"new payload")
    assert (src / victim).read_bytes() == b"stock"
