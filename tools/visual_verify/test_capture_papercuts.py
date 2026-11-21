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


# ---- 2. shader sides --------------------------------------------------------------------------------------------------
def _tree(root: Path, spv: bytes, exe="mc2.exe"):
    (root / "shaders" / "vulkan").mkdir(parents=True, exist_ok=True)
    (root / "shaders" / "vulkan" / "x.comp.spv").write_bytes(spv)
    (root / exe).write_bytes(b"exe")
    return root / exe


def test_exe_with_its_own_differing_shaders_gets_them(tmp_path):
    inst, other = tmp_path / "inst", tmp_path / "other"
    _tree(inst, b"base")
    exe_a = _tree(other, b"changed")            # A's exe was deployed with other shaders; B's exe has none beside it
    exe_b = tmp_path / "build" / "mc2.exe"
    exe_b.parent.mkdir()
    exe_b.write_bytes(b"exe")
    sa, sb, notes = ab.shader_side_plan(inst, exe_a, exe_b, commit_shaders_differ=True)
    assert sa == other and sb is None and any("beside" in n for n in notes)


def test_same_shaders_both_sides_with_differing_commits_is_refused(tmp_path):
    inst = tmp_path / "inst"
    exe_a = _tree(inst, b"base")
    exe_b = tmp_path / "build" / "mc2.exe"
    exe_b.parent.mkdir()
    exe_b.write_bytes(b"exe")
    with pytest.raises(SystemExit) as exc:
        ab.shader_side_plan(inst, exe_a, exe_b, commit_shaders_differ=True)
    assert "SAME shader set" in str(exc.value) and "--shaders-a" in str(exc.value)


def test_same_shaders_is_fine_when_the_commits_do_not_differ_in_shaders(tmp_path):
    inst = tmp_path / "inst"
    exe_a = _tree(inst, b"base")
    sa, sb, notes = ab.shader_side_plan(inst, exe_a, exe_a, commit_shaders_differ=False)
    assert (sa, sb) == (None, None) and notes


def test_explicit_shaders_on_both_sides_pass_the_guard(tmp_path):
    inst, da, db = tmp_path / "inst", tmp_path / "da", tmp_path / "db"
    exe = _tree(inst, b"base")
    _tree(da, b"one")
    _tree(db, b"two")
    sa, sb, _ = ab.shader_side_plan(inst, exe, exe, shaders_a=da, shaders_b=db / "shaders", commit_shaders_differ=True)
    assert (sa, sb) == (da, db)


def test_commits_differ_in_shaders_reads_the_diff(monkeypatch):
    monkeypatch.setattr(ab, "git_diff_files", lambda a, b: {"code/x.cpp", "shaders/vulkan/y.frag"})
    assert ab.commits_differ_in_shaders("a", "b") is True
    monkeypatch.setattr(ab, "git_diff_files", lambda a, b: {"code/x.cpp"})
    assert ab.commits_differ_in_shaders("a", "b") is False
    monkeypatch.setattr(ab, "git_diff_files", lambda a, b: None)
    assert ab.commits_differ_in_shaders("a", "b") is None and ab.commits_differ_in_shaders(None, "b") is None


# ---- 1b. ops-dir bookmark sets (MC2_OPS_BOOKMARKS) -----------------------------------------------------------------------
def test_ops_set_resolves_by_name_and_path_with_collision_order(tmp_path, monkeypatch):
    install, repo, ops, out = tmp_path / "inst", tmp_path / "repo", tmp_path / "ops", tmp_path / "out"
    _bm(install / "saved_bookmarks", "dup", "install")
    _bm(repo, "dup", "repo")
    _bm(repo, "dup2", "repo")
    _bm(ops, "dup", "ops")
    _bm(ops, "dup2", "ops")
    _bm(ops, "only_ops", "{\"o\": 1}")
    monkeypatch.setenv("MC2_OPS_BOOKMARKS", str(ops))
    files, notes, missing = capture.resolve_bookmarks(install, {"dup", "dup2", "only_ops", str(ops / "dup2.json")}, out, repo_dir=repo)
    assert missing == []
    texts = {p.stem: p.read_text(encoding="utf-8") for p in files}
    assert texts["dup"] == "install" and texts["only_ops"] == "{\"o\": 1}"
    assert sorted(p.stem for p in files) == ["dup", "dup2", "dup2", "only_ops"]   # name and explicit path to dup2
    assert any(str(ops / "only_ops.json") in n for n in notes) and any(str(repo / "dup2.json") in n for n in notes)
    assert not (install / "saved_bookmarks" / "only_ops.json").exists()          # install never written


def test_ops_dir_missing_is_skipped_and_not_found_lists_dirs(tmp_path, monkeypatch):
    sys.path.insert(0, str(capture.ROOT / "scripts"))
    import bookmark_sets
    monkeypatch.setenv("MC2_OPS_BOOKMARKS", str(tmp_path / "no_such_dir"))
    install, repo = tmp_path / "inst", tmp_path / "repo"
    _bm(repo, "r")
    assert bookmark_sets.resolve_set("r", install, repo)[0] == repo / "r.json"
    path, dirs = bookmark_sets.resolve_set("zzz", install, repo)
    assert path is None
    msg = bookmark_sets.not_found_message("zzz", dirs)
    assert all(str(d) in msg for d in (install / "saved_bookmarks", repo, tmp_path / "no_such_dir"))


def test_ab_helpers_accept_an_ops_set_without_writing_the_install(tmp_path, monkeypatch):
    ops, install, ref = tmp_path / "ops", tmp_path / "inst", tmp_path / "ref"
    _bm(ops, "ops_only_set")
    monkeypatch.setenv("MC2_OPS_BOOKMARKS", str(ops))
    (install / "data" / "textures").mkdir(parents=True)
    (install / "data" / "textures" / "x.burnin.tga").write_bytes(b"x")
    ab.seed_install(install, str(ops / "ops_only_set.json"), ref)       # path spec, no SystemExit
    ab.seed_install(install, "ops_only_set", ref)                       # bare name
    ab.ensure_saved_bookmarks(install, ref, [], bookmarks="ops_only_set")
    assert not (install / "saved_bookmarks").exists()
    assert ab.bookmark_parity_problems(install, install, str(ops / "ops_only_set.json")) == []
