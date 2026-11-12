"""contracts_cache.py: a key must move when (and only when) a row's inputs move."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import console_child  # noqa: E402
import contracts_cache as cc  # noqa: E402


def test_merge_triggers_use_the_existing_table():
    from pathlib import Path
    import merge_gate
    import run_tools_tests as rtt
    root = str(Path(__file__).resolve().parents[2])
    assert not ({"kit_art", "script_callers", "audio_music_stream", "vk_shaders_compile"}
                & cc.affected_rows(root, ["docs/merge-queue-v2.md"]))
    assert "vk_shaders_compile" in cc.affected_rows(root, ["shaders/vulkan/mech.vert"])
    assert "kit_art" in cc.affected_rows(root, ["tools/scifi_kit/cook.py"])
    assert merge_gate.laptop_sim_rows(["docs/mc2_10-notes.md"]) == []
    assert len(merge_gate.laptop_sim_rows(["code/mission_update.cpp"])) == 6
    modules = ["tools/terrain_beautify/test_cook.py", "tests/terrain_gen/test_generate.py",
               "tests/assets/test_replace.py", "tests/smoke/test_runner.py",
               "editor/tests/test_placement_proofs.py", "tools/scifi_kit/test_kit.py",
               "scripts/tests/test_board_selftests.py", "scripts/tests/test_contract_coverage.py"]
    assert rtt.scoped_modules(modules, ["docs/merge-queue-v2.md"]) == []
    for source, module in [("tools/terrain_beautify/cook.py", modules[0]),
                           ("tools/terrain_gen/generate.py", modules[1]),
                           ("tools/assets/replace.py", modules[2]),
                           ("scripts/smoke_lib/runner.py", modules[3]),
                           ("editor/placement.cpp", modules[4]),
                           ("tools/scifi_kit/cook.py", modules[5]),
                           ("scripts/check-contracts.sh", modules[6])]:
        assert module in rtt.scoped_modules(modules, [source])
    assert rtt.scoped_modules(modules, [modules[2]]) == [modules[2]]


def test_art_input_runs_the_whole_art_suite(monkeypatch):
    import run_tools_tests as rtt
    mod = "tools/scifi_kit/test_kit.py"
    monkeypatch.setattr(sys, "argv", ["run_tools_tests.py", "--suite", "art", "--base", "base"])
    monkeypatch.setattr(rtt, "discover", lambda *a: [mod])
    monkeypatch.setattr(rtt, "art_changed", lambda *a: True)
    monkeypatch.setattr(rtt, "load_ledger", lambda: {})
    monkeypatch.setattr(rtt, "run_all", lambda mods, *a: [(m, 0, [], "") for m in mods])
    assert rtt.main() == 0


def _git(repo, *a):
    subprocess.run(["git", "-C", str(repo), *a], check=True, capture_output=True,
                   **console_child.child_kwargs(redirected=True))


def _repo(tmp_path):
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "config", "user.email", "t@t")
    _git(tmp_path, "config", "user.name", "t")
    (tmp_path / "scripts").mkdir()
    (tmp_path / "src").mkdir()
    (tmp_path / "other").mkdir()
    (tmp_path / "scripts" / "chk.py").write_text("import helper\nprint('x')\n")
    (tmp_path / "scripts" / "helper.py").write_text("X = 1\n")
    (tmp_path / "scripts" / "chk.baseline.json").write_text("{}")
    (tmp_path / "src" / "a.cpp").write_text("int a;\n")
    (tmp_path / "other" / "b.txt").write_text("b\n")
    (tmp_path / "scripts" / "contracts-cache-inputs.txt").write_text("rowa: src\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-qm", "init")
    return tmp_path


def _key(repo, cmd="py -3 scripts/chk.py"):
    uni = cc.Universe(str(repo))
    return cc.compute_key(str(repo), uni, cmd, cc.load_decls(str(repo))["rowa"])


def test_key_follows_inputs_only(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    assert _key(r) == k0
    (r / "other" / "b.txt").write_text("changed\n")        # not an input
    assert _key(r) == k0
    (r / "src" / "a.cpp").write_text("int a2;\n")           # declared scan root, unstaged edit
    k1 = _key(r)
    assert k1 != k0
    (r / "src" / "new.cpp").write_text("int n;\n")          # untracked file in the root
    assert _key(r) != k1
    (r / "src" / "new.cpp").unlink()
    (r / "src" / "a.cpp").write_text("int a;\n")
    assert _key(r) == k0                                    # back to the original bytes


def test_check_code_is_an_input(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    (r / "scripts" / "helper.py").write_text("X = 2\n")             # imported module
    assert _key(r) != k0
    (r / "scripts" / "helper.py").write_text("X = 1\n")
    (r / "scripts" / "chk.baseline.json").write_text('{"a":1}')     # same-stem sibling baseline
    assert _key(r) != k0
    (r / "scripts" / "chk.baseline.json").write_text("{}")
    assert _key(r) == k0
    assert _key(r, "py -3 scripts/chk.py --other") != k0            # the command is part of the key


def test_deleted_input_and_undeclared_row(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    os.remove(r / "src" / "a.cpp")
    assert _key(r) != k0
    assert "nope" not in cc.load_decls(str(r))      # an undeclared row has no declaration, so it is never keyed


def test_touch_without_content_change_keeps_key(tmp_path):
    r = _repo(tmp_path)
    k0 = _key(r)
    f = r / "src" / "a.cpp"
    f.write_text("int a;\n")                  # same bytes, fresh mtime: git sees it as stat-dirty until refreshed
    os.utime(f, (1, 1))
    assert _key(r) == k0
