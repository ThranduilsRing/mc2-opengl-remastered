"""saved_bookmarks is written by the game and by deploy, so a hardlinked bookmark between installs must be flagged."""
import importlib.util
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("check_hardlink_isolation", ROOT / "scripts/check-hardlink-isolation.py")
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)


def test_saved_bookmarks_is_mutated_class():
    assert chk.classify("saved_bookmarks/mc2_01_saved_205644.json") == "mutated"


def test_hardlinked_saved_bookmark_is_flagged(tmp_path):
    a, b = tmp_path / "lane-A", tmp_path / "lane-B"
    for d in (a, b):
        (d / "saved_bookmarks").mkdir(parents=True)
    (a / "saved_bookmarks" / "bm.json").write_text("{}")
    os.link(a / "saved_bookmarks" / "bm.json", b / "saved_bookmarks" / "bm.json")
    verdict, facts, _ = chk.run_check(str(b))
    assert verdict == "SHARED"
    assert any(s["rel"] == "saved_bookmarks/bm.json" for s in facts["shared"])
