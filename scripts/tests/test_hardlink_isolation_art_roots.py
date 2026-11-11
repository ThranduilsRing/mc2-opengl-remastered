"""data/art/ui and data/art/pilots are written by the game (chrome re-pack, pilot portraits): they must stay mutated roots."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("check_hardlink_isolation_art", ROOT / "scripts/check-hardlink-isolation.py")
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)


def test_art_ui_and_pilots_are_mutated_roots():
    assert "data/art/ui" in chk.MUTATED_ROOTS
    assert "data/art/pilots" in chk.MUTATED_ROOTS


def test_art_ui_and_pilots_files_classify_mutated():
    assert chk.classify("data/art/ui/mcui_universal1/btn.png") == "mutated"
    assert chk.classify("data/art/pilots/01.png") == "mutated"
