"""What: tests for split_map.py - the map is deterministic, lists every piece of a known split, and --check notices a stale file.
Why: the map is a contract row; a silent generator bug would make the row green while the map lies."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import split_map


def test_build_is_deterministic():
    assert split_map.build() == split_map.build()


def test_known_split_lists_core_pieces_and_header():
    text = split_map.build()
    assert "## `code/objmgr.cpp`" in text
    for piece in ("objmgr.cpp", "objmgr_io.cpp", "objmgr_update.cpp", "objmgr_internal.h"):
        assert "| `%s` |" % piece in text


def test_check_flags_a_stale_map(tmp_path, monkeypatch):
    stale = tmp_path / "split-map.md"
    stale.write_text("# Split map\n", encoding="utf-8")
    monkeypatch.setattr(split_map, "OUT", str(stale))
    monkeypatch.setattr(sys, "argv", ["split_map.py", "--check"])
    assert split_map.main() == 1
    monkeypatch.setattr(sys, "argv", ["split_map.py", "--write"])
    assert split_map.main() == 0
    monkeypatch.setattr(sys, "argv", ["split_map.py", "--check"])
    assert split_map.main() == 0
