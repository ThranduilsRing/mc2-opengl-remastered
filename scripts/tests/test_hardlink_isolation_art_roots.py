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


def test_art_gui_is_one_mutated_root_not_a_file_list():
    """W147 Argon review: the 17 campaign chrome files listed in MUTATED_FILES became the data/art/gui root."""
    assert "data/art/gui" in chk.MUTATED_ROOTS
    assert not [f for f in chk.MUTATED_FILES if f.startswith("data/art/gui")]
    for rel in ("data/art/gui/mcl_cm_op1_1.tga", "data/art/gui/mcl_cm_op3_4.tga", "data/art/gui/mcl_cm_5.png",
                "data/art/gui/mcl_mn_4.tga", "data/art/gui/some_future_sheet.tga"):
        assert chk.classify(rel) == "mutated"
