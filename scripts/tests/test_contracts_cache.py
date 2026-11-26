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
    assert "contracts_cache" in cc.affected_rows(root, [".gitignore"])
    assert merge_gate.laptop_sim_rows(["docs/mc2_10-notes.md"]) == []
    assert len(merge_gate.laptop_sim_rows(["code/mission_update.cpp"])) == 2
    modules = ["tools/terrain_beautify/test_cook.py", "tests/terrain_gen/test_generate.py",
               "tests/assets/test_replace.py", "tests/smoke/test_runner.py",
               "editor/tests/test_placement_proofs.py", "tools/scifi_kit/test_kit.py",
               "scripts/tests/test_board_selftests.py", "scripts/tests/test_contract_coverage.py"]
    assert rtt.scoped_modules(modules, ["docs/merge-queue-v2.md"]) == []
    assert rtt.scoped_modules(modules + ["scripts/tests/test_contracts_cache.py"], [".gitignore"]) == ["scripts/tests/test_contracts_cache.py"]
    for source, module in [("tools/terrain_beautify/cook.py", modules[0]),
                           ("tools/terrain_gen/generate.py", modules[1]),
                           ("tools/assets/replace.py", modules[2]),
                           ("scripts/smoke_lib/runner.py", modules[3]),
                           ("editor/placement.cpp", modules[4]),
                           ("tools/scifi_kit/cook.py", modules[5]),
                           ("scripts/check-contracts.sh", modules[6])]:
        assert module in rtt.scoped_modules(modules, [source])
    assert rtt.scoped_modules(modules, [modules[2]]) == [modules[2]]


def test_art_input_runs_its_selected_module(monkeypatch):
    import run_tools_tests as rtt
    mod = "tools/scifi_kit/test_kit.py"
    monkeypatch.setattr(sys, "argv", ["run_tools_tests.py", "--suite", "art", "--base", "base"])
    monkeypatch.setattr(rtt, "discover", lambda *a: [mod])
    monkeypatch.setattr(rtt, "art_changed", lambda *a: True)
    monkeypatch.setattr(rtt.subprocess, "check_output", lambda *a, **k: "tools/scifi_kit/cook.py\0")
    monkeypatch.setattr(rtt, "load_ledger", lambda: {})
    monkeypatch.setattr(rtt, "run_all", lambda mods, *a: [(m, 0, [], "") for m in mods])
    assert rtt.main() == 0


def test_unmapped_change_is_reported_and_art_is_local(capsys):
    import run_tools_tests as rtt
    modules = ["tools/scifi_kit/test_kit.py", "tools/blender_pipeline/test_export.py"]
    assert rtt.scoped_modules(modules, ["new_subsystem/input.xyz"]) == []
    assert "new_subsystem/input.xyz" in capsys.readouterr().out
    assert rtt.scoped_modules(modules, ["tools/scifi_kit/cook.py"]) == modules[:1]
    assert rtt.scoped_modules(modules, ["tools/blender_pipeline/export.py"]) == modules[1:]
    assert rtt.scoped_modules(modules, ["data/new.glb"]) == modules


def test_1122m_selection_is_small_and_includes_campaign_marker():
    import run_tools_tests as rtt
    import run_scripts_tests as rst
    paths = subprocess.run(["git", "diff", "--name-only", "8fc8afe78", "7fb0d52f7"], cwd=rtt.ROOT, text=True,
                           check=True, stdout=subprocess.PIPE,
                           **console_child.child_kwargs(redirected=True)).stdout.splitlines()
    modules = rtt.discover() + rst.discover()
    selected = rtt.scoped_modules(modules, paths)
    assert len(modules) == 433, f"{len(modules)} test modules discovered: bump this pin with the module you added or removed"
    assert len(selected) == 10
    assert "scripts/tests/test_campaign_marker_selection.py" in selected


def test_exact_tip_passes_reject_dirty_moved_failed_and_corrupt(tmp_path, monkeypatch):
    repo = _repo(tmp_path)
    monkeypatch.setattr(cc, "cache_path", lambda *a: str(tmp_path / ".git/pass-cache.json"))
    head = cc.clean_tip(repo)
    assert not cc.tip_passed(repo, "lint")
    cc.record_tip_pass(repo, "lint", head, True)
    assert cc.tip_passed(repo, "lint")
    (repo / "src/a.cpp").write_text("changed")
    assert not cc.tip_passed(repo, "lint")
    _git(repo, "add", "src/a.cpp")
    _git(repo, "commit", "-qm", "next")
    assert not cc.tip_passed(repo, "lint")
    cc.record_tip_pass(repo, "lint", cc.clean_tip(repo), True)
    cc.record_tip_pass(repo, "lint", cc.clean_tip(repo), False)
    assert not cc.tip_passed(repo, "lint")
    (repo / ".git/pass-cache.json").write_text('{"exact-tip:lint": false}')
    assert not cc.tip_passed(repo, "lint")


def test_equivalents_and_stock_trigger_paths(monkeypatch):
    from pathlib import Path
    root = str(Path(__file__).resolve().parents[2])
    monkeypatch.setenv("CONTRACTS_NO_CACHE", "0")
    monkeypatch.setattr(cc, "tip_passed", lambda repo, label: False)
    paths = ["tools/callers.py", "code/test.cpp", "tests/unit/test_audio_backend.cpp"]
    assert {"script_callers", "log_tag_census", "audio_music_stream"} <= cc.affected_rows(root, paths)
    monkeypatch.setattr(cc, "tip_passed", lambda repo, label: True)
    assert not {"script_callers", "log_tag_census", "audio_music_stream"} & cc.affected_rows(root, paths)
    assert "stock_mission_data" in cc.affected_rows(root, ["tools/new_tool.py"])
    assert "stock_mission_data" not in cc.affected_rows(root, ["data/terrain.fit"])


# Trigger: scripts/contracts_cache.py and check-contracts.sh --no-cache export.
def test_forced_execution_keeps_exact_tip_equivalents(monkeypatch):
    from pathlib import Path
    root = Path(__file__).resolve().parents[2]
    monkeypatch.setattr(cc, "tip_passed", lambda *a: True)
    monkeypatch.setenv("CONTRACTS_NO_CACHE", "1")
    paths = ["tools/callers.py", "code/test.cpp", "tests/unit/test_audio_backend.cpp"]
    assert {"script_callers", "log_tag_census", "audio_music_stream"} <= cc.affected_rows(str(root), paths)
    shell = (root / "scripts/check-contracts.sh").read_text()
    assert '--no-cache) NO_CACHE=1' in shell
    assert shell.index('export CONTRACTS_NO_CACHE=1') < shell.index('contracts_cache.py" affected')


def test_generators_follow_inputs_including_deletions(tmp_path, monkeypatch):
    from tools import lane_merge as lm
    repo = _repo(tmp_path)
    monkeypatch.setattr(lm, "ARTIFACTS", [])
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "scripts/chk.py", [])])
    monkeypatch.setattr(lm, "GENERATOR_ROWS", {"fixture": "rowa"})
    # regenerate_all's fixture guard only checks that the first generator exists.
    calls = []
    monkeypatch.setattr(lm, "run", lambda root, cmd: calls.append(cmd) or type("R", (), {"returncode": 0, "stdout": ""})())
    base = cc.clean_tip(repo)
    (repo / "other/b.txt").write_text("unrelated")
    _git(repo, "add", "other/b.txt"); _git(repo, "commit", "-qm", "unrelated")
    assert lm.regenerate_all(str(repo), base=base) == [] and not calls
    (repo / "src/a.cpp").unlink()
    _git(repo, "add", "src/a.cpp"); _git(repo, "commit", "-qm", "deleted input")
    lm.regenerate_all(str(repo), base=base)
    assert len(calls) == 1


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
