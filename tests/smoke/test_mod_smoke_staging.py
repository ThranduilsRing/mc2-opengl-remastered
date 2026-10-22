"""Disposable mod staging keeps runtime fonts, stock gaps and mod overrides."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import mod_smoke


def _write_fst(path, members):
    offset = 8 + 266 * len(members)
    table, blobs = b"", b""
    for name, data in members.items():
        table += struct.pack("<IIII250s", offset + len(blobs), len(data), len(data), 0, name.encode())
        blobs += data
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<II", 0xCADDECAF, len(members)) + table + blobs)


def test_stage_preserves_mod_assets_and_supplies_missing_runtime_fonts(tmp_path):
    base, mod, staged = (tmp_path / name for name in ("base", "mod", "staged"))
    for root in (base, mod):
        (root / "assets/graphics").mkdir(parents=True)
    (mod / "data/missions").mkdir(parents=True)
    (mod / "data/missions/first.fit").write_bytes(b"mission")
    (base / "assets/graphics/arial8.bmp").write_bytes(b"base-font")
    (base / "assets/graphics/shared.bmp").write_bytes(b"base-style")
    (mod / "assets/graphics/shared.bmp").write_bytes(b"mod-style")
    (mod / "assets/graphics/custom.bmp").write_bytes(b"mod-only")
    _write_fst(base / "camera.fst", {"data/cameras/cameras.fit": b"stock-camera"})
    _write_fst(mod / "camera.fst", {"data/cameras/custom.fit": b"mod-camera"})
    (base / "system.cfg").write_text(
        '[FastFiles]\nl NumFastFiles=1\nst File0="camera.fst"\n\n[After]\nst Keep="yes"\n',
        encoding="utf-8")
    before = {root: mod_smoke.tree_identity(root) for root in (base, mod)}

    mod_smoke.stage_mod(base, [mod], staged, "first", base)

    assert (staged / "assets/graphics/arial8.bmp").read_bytes() == b"base-font"
    assert (staged / "assets/graphics/shared.bmp").read_bytes() == b"mod-style"
    assert (staged / "assets/graphics/custom.bmp").read_bytes() == b"mod-only"
    assert (base / "assets/graphics/shared.bmp").read_bytes() == b"base-style"
    assert (staged / "data/missions/first.fit").read_bytes() == b"mission"
    assert (staged / "camera.fst").read_bytes() == (mod / "camera.fst").read_bytes()
    assert (staged / "data/cameras/cameras.fit").read_bytes() == b"stock-camera"
    assert not (staged / "data/cameras/custom.fit").exists()
    config = (staged / "system.cfg").read_bytes()
    assert config == (base / "system.cfg").read_bytes()
    mod_smoke.stage_mod(base, [mod], staged, "first", base)
    assert (staged / "system.cfg").read_bytes() == config
    assert {root: mod_smoke.tree_identity(root) for root in (base, mod)} == before


def test_carver_campaign_layers_on_omnitech_without_mutating_sources(tmp_path):
    base, mods, staged = (tmp_path / name for name in ("base", "mods", "staged"))
    _write_fst(base / "camera.fst", {"data/cameras/cameras.fit": b"stock-camera"})
    parent = mods / "MechCommanderOmnitech"
    overlay = mods / "MCO-Carver5"
    for root, stem in ((parent, "a_0_1"), (overlay, "mc2_01")):
        (root / "data/missions").mkdir(parents=True)
        (root / f"data/missions/{stem}.fit").write_bytes(stem.encode())
    (parent / "data/objects.pak").write_bytes(b"omnitech-types")
    (parent / "data/shared.fit").write_bytes(b"parent")
    (overlay / "data/shared.fit").write_bytes(b"campaign-override")
    _write_fst(parent / "camera.fst", {"data/cameras/mcocameras.fit": b"omnitech-camera"})
    before = {root: mod_smoke.tree_identity(root) for root in (base, mods)}
    layers = mod_smoke.mod_layers(mods, "MCO-Carver5", "MechCommanderOmnitech")

    mod_smoke.stage_mod(base, layers, staged, "mc2_01", base)

    assert (staged / "data/objects.pak").read_bytes() == b"omnitech-types"
    assert (staged / "data/shared.fit").read_bytes() == b"campaign-override"
    assert (staged / "data/missions/mc2_01.fit").read_bytes() == b"mc2_01"
    assert (staged / "camera.fst").read_bytes() == (parent / "camera.fst").read_bytes()
    assert (staged / "data/cameras/cameras.fit").read_bytes() == b"stock-camera"
    assert {root: mod_smoke.tree_identity(root) for root in (base, mods)} == before
