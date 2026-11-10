"""What: capture.py / ab.py papercuts (repo bookmark fallback, unregistered-knob guard, A/B shader sides).
Why: lane runs were lost to a bookmark that exists in tests/visual/bookmarks/ but not in the install, to a knob the exe never
registered, and to an A/B that loaded one shader set for both sides. Fakes only: no engine is run."""
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab  # noqa: E402
import capture  # noqa: E402


# ---- 1. bookmarks -----------------------------------------------------------------------------------------------------
def _bm(d: Path, stem: str, text="{}"):
    d.mkdir(parents=True, exist_ok=True)
    (d / (stem + ".json")).write_text(text, encoding="utf-8")


def test_repo_bookmark_fallback_copies_and_never_writes_the_install(tmp_path):
    install, repo, out = tmp_path / "inst", tmp_path / "repo", tmp_path / "out"
    _bm(install / "saved_bookmarks", "in_install", "{\"a\": 1}")
    _bm(repo, "only_repo", "{\"b\": 2}")
    files, notes, missing = capture.resolve_bookmarks(install, {"in_install", "only_repo"}, out, repo_dir=repo)
    assert missing == []
    by = {p.stem: p for p in files}
    assert by["in_install"].parent == install / "saved_bookmarks"
    assert by["only_repo"].parent == out / "_repo_bookmarks"          # a copy in a dir this run owns, not the repo file
    assert by["only_repo"].read_text(encoding="utf-8") == "{\"b\": 2}"
    assert len(notes) == 1 and "only_repo" in notes[0] and str(repo / "only_repo.json") in notes[0]
    assert sorted(p.name for p in (install / "saved_bookmarks").iterdir()) == ["in_install.json"]


def test_install_bookmark_wins_over_repo_and_unknown_is_reported(tmp_path):
    install, repo, out = tmp_path / "inst", tmp_path / "repo", tmp_path / "out"
    _bm(install / "saved_bookmarks", "same", "install")
    _bm(repo, "same", "repo")
    files, notes, missing = capture.resolve_bookmarks(install, {"same", "nowhere"}, out, repo_dir=repo)
    assert [p.read_text(encoding="utf-8") for p in files] == ["install"] and notes == [] and missing == ["nowhere"]


def test_all_means_install_only(tmp_path):
    install, repo = tmp_path / "inst", tmp_path / "repo"
    _bm(install / "saved_bookmarks", "a")
    _bm(repo, "b")
    files, _, _ = capture.resolve_bookmarks(install, None, tmp_path / "out", repo_dir=repo)
    assert [p.stem for p in files] == ["a"]


def test_seed_install_accepts_a_repo_only_bookmark(tmp_path, monkeypatch):
    repo_bm = ab.ROOT / "tests" / "visual" / "bookmarks"
    stem = sorted(p.stem for p in repo_bm.glob("*.json"))[0]
    install, ref = tmp_path / "inst", tmp_path / "ref"
    (install / "data" / "textures").mkdir(parents=True)
    (install / "data" / "textures" / "x.burnin.tga").write_bytes(b"x")
    ab.seed_install(install, stem, ref)                                 # no SystemExit: capture.py falls back to the repo copy
    assert not (install / "saved_bookmarks").exists()                   # and ab.py wrote nothing into the install


def test_seed_install_still_names_a_bookmark_found_nowhere(tmp_path):
    install = tmp_path / "inst"
    (install / "data" / "textures").mkdir(parents=True)
    (install / "data" / "textures" / "x.burnin.tga").write_bytes(b"x")
    with pytest.raises(SystemExit) as exc:
        ab.seed_install(install, "no_such_bookmark_anywhere", tmp_path / "ref")
    assert "no_such_bookmark_anywhere" in str(exc.value)
