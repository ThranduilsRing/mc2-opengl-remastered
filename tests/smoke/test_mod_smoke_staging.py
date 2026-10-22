"""Disposable mod staging keeps runtime fonts and mod overrides."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import mod_smoke


def test_stage_preserves_mod_assets_and_supplies_missing_runtime_fonts(tmp_path):
    base, mod, staged = (tmp_path / name for name in ("base", "mod", "staged"))
    for root in (base, mod):
        (root / "assets/graphics").mkdir(parents=True)
    for name in mod_smoke.STOCK_FSTS + mod_smoke.CONFIG_FILES:
        (base / name).write_bytes(b"fixture")
    (mod / "data/missions").mkdir(parents=True)
    (mod / "data/missions/first.fit").write_bytes(b"mission")
    (base / "assets/graphics/arial8.bmp").write_bytes(b"base-font")
    (base / "assets/graphics/shared.bmp").write_bytes(b"base-style")
    (mod / "assets/graphics/shared.bmp").write_bytes(b"mod-style")
    (mod / "assets/graphics/custom.bmp").write_bytes(b"mod-only")
    (mod / "camera.fst").write_bytes(b"mod-camera")
    staged.mkdir()
    (staged / "system.cfg").write_text(
        '[FastFiles]\nl NumFastFiles=1\nst File0="camera.fst"\n\n[After]\nst Keep="yes"\n',
        encoding="utf-8")
    before = {root: mod_smoke.tree_identity(root) for root in (base, mod)}

    mod_smoke.stage_mod(mod, staged, "first", "loose", base)

    assert (staged / "assets/graphics/arial8.bmp").read_bytes() == b"base-font"
    assert (staged / "assets/graphics/shared.bmp").read_bytes() == b"mod-style"
    assert (staged / "assets/graphics/custom.bmp").read_bytes() == b"mod-only"
    assert (staged / "data/missions/first.fit").read_bytes() == b"mission"
    assert {root: mod_smoke.tree_identity(root) for root in (base, mod)} == before

    assert (staged / "camera.fst").read_bytes() == b"mod-camera"
    assert (staged / "stock_fallback/camera.fst").read_bytes() == b"fixture"
    config = (staged / "system.cfg").read_text(encoding="utf-8")
    assert 'l NumFastFiles = 9' in config
    assert 'st File0="camera.fst"' in config
    assert 'st File1="stock_fallback/mission.fst"' in config
    assert '[After]\nst Keep="yes"' in config
    mod_smoke.stage_mod(mod, staged, "first", "loose", base)
    assert (staged / "system.cfg").read_text(encoding="utf-8") == config
    assert {root: mod_smoke.tree_identity(root) for root in (base, mod)} == before


def test_carver_campaign_layers_on_omnitech_without_mutating_sources(tmp_path):
    base, mods, staged = (tmp_path / name for name in ("base", "mods", "staged"))
    base.mkdir()
    for name in mod_smoke.STOCK_FSTS + mod_smoke.CONFIG_FILES:
        (base / name).write_bytes(b"stock")
    parent = mods / "MechCommanderOmnitech"
    overlay = mods / "MCO-Carver5"
    for root, stem in ((parent, "a_0_1"), (overlay, "mc2_01")):
        (root / "data/missions").mkdir(parents=True)
        (root / f"data/missions/{stem}.fit").write_bytes(stem.encode())
    (parent / "data/objects.pak").write_bytes(b"omnitech-types")
    (parent / "data/shared.fit").write_bytes(b"parent")
    (overlay / "data/shared.fit").write_bytes(b"campaign-override")
    (parent / "camera.fst").write_bytes(b"omnitech-camera")
    before = {root: mod_smoke.tree_identity(root) for root in (base, mods)}
    staged.mkdir()
    (staged / "system.cfg").write_text(
        '[FastFiles]\nl NumFastFiles=1\nst File0="camera.fst"\n', encoding="utf-8")

    stats = mod_smoke.stage_battery_mod(mods, "MCO-Carver5", staged,
                                      "mc2_01", "loose", base)

    assert stats["parent_mod"] == "MechCommanderOmnitech"
    assert (staged / "data/objects.pak").read_bytes() == b"omnitech-types"
    assert (staged / "data/shared.fit").read_bytes() == b"campaign-override"
    assert (staged / "data/missions/mc2_01.fit").read_bytes() == b"mc2_01"
    assert (staged / "camera.fst").read_bytes() == b"omnitech-camera"
    assert {root: mod_smoke.tree_identity(root) for root in (base, mods)} == before
