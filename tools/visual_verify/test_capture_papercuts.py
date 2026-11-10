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


# ---- 3. unregistered env ----------------------------------------------------------------------------------------------
def _exe(tmp_path, names):
    p = tmp_path / "mc2.exe"
    p.write_bytes(b"MZ\0\0" + b"\0".join(n.encode() for n in names) + b"\0tail")
    return p


def test_exe_knows_env_finds_names_across_chunk_edges(tmp_path, monkeypatch):
    exe = _exe(tmp_path, ["MC2_VISUAL_CAPTURE_CLEAN"])
    assert capture.exe_knows_env(exe, ["MC2_VISUAL_CAPTURE_CLEAN", "MC2_VISUAL_LIVE_SETTLE_SEC"]) == {
        "MC2_VISUAL_CAPTURE_CLEAN": True, "MC2_VISUAL_LIVE_SETTLE_SEC": False}
    big = tmp_path / "big.exe"            # the name straddles a read boundary
    big.write_bytes(b"x" * ((1 << 24) - 5) + b"MC2_VISUAL_LIVE_SETTLE_SEC" + b"y" * 100)
    assert capture.exe_knows_env(big, ["MC2_VISUAL_LIVE_SETTLE_SEC"]) == {"MC2_VISUAL_LIVE_SETTLE_SEC": True}


def test_flag_knob_the_exe_lacks_is_fatal_and_an_env_knob_warns(tmp_path):
    exe = _exe(tmp_path, ["MC2_VISUAL_CAPTURE_CLEAN"])
    declared = {"MC2_VISUAL_CAPTURE_CLEAN": "1", "MC2_VISUAL_LIVE_SETTLE_SEC": "5", "MC2_EXTRA_KNOB": "1", "NOT_MC2": "1"}
    fatal, warn = capture.env_registration_problems(exe, declared, {"MC2_VISUAL_CAPTURE_CLEAN", "MC2_VISUAL_LIVE_SETTLE_SEC"})
    assert len(fatal) == 1 and "MC2_VISUAL_LIVE_SETTLE_SEC" in fatal[0]
    assert len(warn) == 1 and "MC2_EXTRA_KNOB" in warn[0]


def test_all_registered_is_quiet(tmp_path):
    exe = _exe(tmp_path, ["MC2_VISUAL_LIVE_SETTLE_SEC"])
    assert capture.env_registration_problems(exe, {"MC2_VISUAL_LIVE_SETTLE_SEC": "5"}, {"MC2_VISUAL_LIVE_SETTLE_SEC"}) == ([], [])
