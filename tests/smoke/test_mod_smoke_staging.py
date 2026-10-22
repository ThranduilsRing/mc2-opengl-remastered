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
    before = {root: mod_smoke.tree_identity(root) for root in (base, mod)}

    mod_smoke.stage_mod(mod, staged, "first", "loose", base)

    assert (staged / "assets/graphics/arial8.bmp").read_bytes() == b"base-font"
    assert (staged / "assets/graphics/shared.bmp").read_bytes() == b"mod-style"
    assert (staged / "assets/graphics/custom.bmp").read_bytes() == b"mod-only"
    assert (staged / "data/missions/first.fit").read_bytes() == b"mission"
    assert {root: mod_smoke.tree_identity(root) for root in (base, mod)} == before
