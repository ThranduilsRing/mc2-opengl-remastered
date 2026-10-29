"""Tests for the merge-window speedups: lane_merge quick rows before the verify, ninja_build oversize-pdb guard (no game, no build)."""
import os
import sys
import types
import subprocess
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
import console_child  # noqa: E402
import ninja_build  # noqa: E402


def _r(rc, out=""):
    return types.SimpleNamespace(returncode=rc, stdout=out)


def test_quick_rows_green_and_red():
    assert lm.quick_rows(ROOT, lambda root, cmd, env=None: _r(0)) == []
    seen = []

    def runner(root, cmd, env=None):
        seen.append(cmd)
        return _r(1 if "pytest" in cmd else 0, "dup owner")
    failed = lm.quick_rows(ROOT, runner)
    assert [f[0] for f in failed] == ["test_ownership"] and failed[0][2] == "dup owner"
    assert len(seen) == 2  # both rows ran


def _throwaway_repo(tmp_path):
    import subprocess

    def git(*a):
        subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=t", "-c", "user.email=t@t", *a], check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
    git("init", "-q", "-b", "main")
    (tmp_path / "a.txt").write_text("a")
    (tmp_path / "ok.py").write_text("import sys; sys.exit(0)")
    (tmp_path / "bad.py").write_text("import sys; print('boom'); sys.exit(1)")
    git("add", "."); git("commit", "-q", "-m", "base")
    git("checkout", "-q", "-b", "lane")
    (tmp_path / "b.txt").write_text("b")
    git("add", "."); git("commit", "-q", "-m", "lane work")
    git("checkout", "-q", "main")
    (tmp_path / "tools").mkdir()
    return tmp_path


def test_batch_exits_6_on_red_quick_row_before_verify(tmp_path, monkeypatch, capsys):
    root = _throwaway_repo(tmp_path)
    verifies = []
    monkeypatch.setattr(lm, "verify_merged", lambda *a, **k: verifies.append(a) or ("OK", []))
    monkeypatch.setattr(lm, "RATCHETS", [])
    monkeypatch.setattr(lm, "QUICK_ROWS", [("good_row", ["ok.py"]), ("bad_row", ["bad.py"])])
    rc = lm.main(["--batch", "lane", "--root", str(root), "--tools-dir", str(root / "tools")])
    out = capsys.readouterr()
    assert rc == 6 and not verifies
    assert "BATCH STOPPED at quick rows (bad_row)" in out.out and "boom" in out.err


def test_batch_runs_verify_when_quick_rows_green(tmp_path, monkeypatch):
    root = _throwaway_repo(tmp_path)
    verifies = []
    monkeypatch.setattr(lm, "verify_merged", lambda *a, **k: verifies.append(a) or ("OK", []))
    monkeypatch.setattr(lm, "RATCHETS", [])
    monkeypatch.setattr(lm, "QUICK_ROWS", [("good_row", ["ok.py"])])
    assert lm.main(["--batch", "lane", "--root", str(root), "--tools-dir", str(root / "tools")]) == 0
    assert len(verifies) == 1


def test_contained_batch_refreshes_window_receipt(tmp_path, monkeypatch):
    root = _throwaway_repo(tmp_path)
    (root / 'scripts').mkdir()
    (root / 'scripts/ninja_build.py').touch()
    monkeypatch.setattr(lm, 'RATCHETS', [])
    monkeypatch.setattr(lm, 'QUICK_ROWS', [])
    verifies = []
    monkeypatch.setattr(lm, 'verify_merged', lambda *a: verifies.append(a) or ('OK', []))
    args = ['--batch', 'lane', '--root', str(root), '--tools-dir', str(root / 'tools')]
    assert lm.main(args) == 0
    receipt = root / 'build64-ninja/merge-window.json'
    original = receipt.read_bytes()
    import json
    window = json.loads(original)
    assert window['base'] != window['head']
    # A fix-forward makes the previous receipt stale while containing the lane.
    (root / 'a.txt').write_text('fixed')
    lm.git_out(str(root), 'add', '--', 'a.txt')
    lm.git_out(str(root), '-c', 'user.name=t', '-c', 'user.email=t@t',
               'commit', '-qm', 'fix forward')
    head = lm.git_out(str(root), 'rev-parse', 'HEAD')
    assert lm.main(args) == 0
    refreshed = json.loads(receipt.read_bytes())
    assert refreshed['head'] == head and refreshed['verify'] == 'OK'
    assert refreshed['head'] != window['head'] and len(verifies) == 2
    receipt.unlink()
    assert lm.main(args) == 0
    assert json.loads(receipt.read_bytes())['head'] == head


@pytest.mark.parametrize('jobs', [1, 6])
def test_contract_board_owns_inner_worker_budget(tmp_path, jobs):
    from pathlib import Path
    script = Path(ROOT, 'scripts/check-contracts.sh').read_text()
    script = script.replace('py -3 scripts/run_scripts_tests.py', r'test \"$MC2_TEST_JOBS\" -eq 1')
    fixture = tmp_path / 'board.sh'
    fixture.write_text(script, encoding='utf-8', newline='\n')
    result = subprocess.run([lm.git_bash(), fixture.as_posix(), '--rows', '^scripts_tests$', '--no-cache', f'-j{jobs}'],
                            cwd=ROOT, env=dict(os.environ, MC2_TEST_JOBS='18'), capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'scripts_tests' in result.stdout


def test_missing_quick_row_script_is_an_error_in_the_real_root_only(tmp_path, monkeypatch):
    never = lambda root, cmd, env=None: _r(0)
    monkeypatch.setattr(lm, "QUICK_ROWS", [("gone", ["tools/no_such_row.py"])])
    assert lm.quick_rows(str(tmp_path), never) == []   # throwaway root: skipped
    failed = lm.quick_rows(os.path.abspath(os.path.join(lm.HERE, "..")), never)
    assert failed and failed[0][0] == "gone" and "missing" in failed[0][2]


def test_real_quick_row_scripts_exist():
    for _, args in lm.QUICK_ROWS:
        for a in args:
            if a.endswith(".py"):
                assert os.path.exists(os.path.join(ROOT, a)), a


def test_oversize_pdb_deleted_small_kept(tmp_path, capsys):
    tree = tmp_path / "t"
    tree.mkdir()
    pdb = tree / "mc2.pdb"
    exe = tree / "mc2.exe"
    exe.write_bytes(b"engine")
    pdb.write_bytes(b"x" * 100)
    assert not ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=1000) and pdb.exists() and exe.exists()
    assert ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=50)
    assert not pdb.exists() and not exe.exists() and "deleted t/mc2.pdb" in capsys.readouterr().out
    assert not ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=50)  # missing pdb: no error


def test_tooling_only_regex_covers_build_inputs():
    for p in ("a/res.rc", "a/x.def", "data/x.ini", "cfg.h.in", "cmake/Find.txt", "scripts/generate_gates.py", "x/CMakeLists.txt"):
        assert lm.COMPILED_INPUTS.search(p), p
    for p in ("tools/lane_merge.py", "docs/a.md", "scripts/tests/test_x.py"):
        assert not lm.COMPILED_INPUTS.search(p), p


@pytest.mark.parametrize("guards,flags,returncode,count", [
    ([False, True], [], 0, 2), ([False, False], [], 0, 1),
    ([], ["-n"], 0, 1), ([False], [], 7, 1),
])
def test_build_compacts_new_growth_without_compiling_twice(monkeypatch, guards, flags, returncode, count):
    monkeypatch.delenv("MC2_NINJA_TREE", raising=False)
    monkeypatch.setattr(sys, "argv", [ninja_build.__file__, "mc2", "-j1", *flags])
    pending = iter(guards)
    monkeypatch.setattr(ninja_build, "drop_oversize_pdb", lambda *a: next(pending))
    monkeypatch.setattr(ninja_build, "tree_uses_cache", lambda *a: False)
    commands = []
    monkeypatch.setattr(ninja_build, "under_vcvars", lambda command, env: commands.append(command) or returncode)
    with pytest.raises(SystemExit) as stopped:
        ninja_build.main()
    assert stopped.value.code == returncode
    assert len(commands) == count and len(set(commands)) == 1


def test_trailer_defaults_from_commit_trailer_env():
    import subprocess
    code = "import sys; sys.path.insert(0, %r); from tools import lane_merge as lm; print(repr(lm.TRAILER))" % ROOT
    env = {k: v for k, v in os.environ.items() if not k.startswith("MC2_")}
    assert subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout.strip() == "''"
    env["MC2_COMMIT_TRAILER"] = "Co-Authored-By: X <x@y>"
    assert "Co-Authored-By: X" in subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout


def test_oversize_pdb_that_cannot_be_deleted_is_reported(tmp_path, monkeypatch, capsys):
    (tmp_path / "t").mkdir()
    (tmp_path / "t" / "mc2.pdb").write_bytes(b"x" * 100)

    def locked(path):
        raise PermissionError(13, "in use")
    monkeypatch.setattr(ninja_build.os, "remove", locked)
    assert not ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=50)
    assert "WARNING: could not delete oversize t/mc2.pdb" in capsys.readouterr().err



def test_merge_ratchets_scan_worktree_before_verify():
    assert len([r for r in lm.RATCHETS if "--worktree" in r[2]]) == 4
    assert lm.verify_env(ROOT)["MC2_NINJA_TREE"] == "build64-ninja" and lm.verify_env(ROOT)["MC2_UNIT_BUILD_DIR"] == os.path.join(ROOT, "build-cc-tests")


def test_deploy_checks_exe_bytes_even_with_fresh_header(tmp_path, capsys):
    import pytest
    import deploy_payload as dp
    head = "a" * 40
    exe = tmp_path / "mc2.exe"
    header = tmp_path / "generated/build_fingerprint.h"
    header.parent.mkdir()
    header.write_text('#define MC2_BUILD_GIT_SHA "' + head[:12] + '"')
    def image(sha):
        return ("MC2FP1|" + sha + "|0|claude/test|2026-10-23T12:00:00|A:/Games/test|FP1END").encode()
    exe.write_bytes(image("b" * 12))
    with pytest.raises(SystemExit):
        dp.require_head_exe(exe, head)
    assert "REFUSING" in capsys.readouterr().err
    exe.write_bytes(b"no fingerprint")
    with pytest.raises(SystemExit):
        dp.require_head_exe(exe, head)
    exe.write_bytes(image(head[:12]))
    assert dp.require_head_exe(exe, head)["sha"] == head[:12]


def test_deploy_stale_refusal_precedes_payload_copy(tmp_path, monkeypatch):
    import pytest
    import deploy_payload as dp
    monkeypatch.setattr(dp, "check_exe_fresh", lambda *a, **k: None)
    monkeypatch.setattr(dp, "git_head", lambda *a: "a" * 40)
    monkeypatch.setattr(dp, "enumerate_payload", lambda *a, **k: pytest.fail("payload reached before provenance guard"))
    (tmp_path / "mc2.exe").write_bytes(b"missing fingerprint")
    args = types.SimpleNamespace(allow_stale_exe=True, build64_root=None)
    with pytest.raises(SystemExit):
        dp.run_deploy(args, str(tmp_path), str(tmp_path), str(tmp_path), "mc2.exe", "mc2.pdb")


def test_gate_plan_uses_verified_ninja_and_required_checks():
    import merge_gate as gate
    steps = gate.plan(6)
    mods = next(cmd for name, cmd, _ in steps if name == "mods")
    assert "--fail-fast" not in mods
    assert mods[mods.index("--workers") + 1] == "2"
    assert len(steps) == 20
    quiet = next(cmd for name, cmd, _ in steps if name == "quiet")
    assert quiet[-3:-1] == ["--fresh-smoke", "--exe"] and quiet[-1].endswith("mc2-win64-v0.4/mc2.exe")
    by_name = {name: (cmd, env) for name, cmd, env in steps}
    assert by_name["contracts"][0][-1] == "-j6"
    assert by_name["harnesses"][0][-2:] == ["-j", "6"]
    assert "--ninja" in by_name["deploy"][0] and "--require-clean" in by_name["deploy"][0]
    assert [name for name, _, _ in steps[:5]] == ["capture-before", "deploy", "capture-after", "compare-captures", "contracts"]
    assert by_name["determinism-mc2_24"][0][2:4] == ["check", "mc2_24"]
    assert not any("ninja_build.py" in arg for _, cmd, _ in steps for arg in cmd)
    for name, cmd, _ in steps:
        if not name.startswith("saveload-"):
            continue
        assert cmd[2] == "check"
        assert "--first-save-only" in cmd
        assert cmd[cmd.index("--mask") + 1] == "tools/save_noise_mask.json"
    assert steps[-1][2] == {"MC2_VULKAN_VALIDATION": "1"}


def test_gate_repeated_before_captures_measure_noise_before_deploy(tmp_path):
    import merge_gate as gate
    steps = gate.plan(6, tmp_path, 3)
    names = [s[0] for s in steps]
    assert names[:6] == ["capture-before", "capture-before-2", "compare-noise-2",
                         "capture-before-3", "compare-noise-3", "deploy"]
    commands = {name: cmd for name, cmd, _ in steps}
    assert commands["capture-before-3"][-1] == str(tmp_path / "before-3")
    assert commands["compare-noise-3"][3:5] == [str(tmp_path / "before"), str(tmp_path / "before-3")]
    with pytest.raises(ValueError, match="positive"):
        gate.plan(6, tmp_path, 0)


def test_lane_failure_summary_keeps_early_case_and_location():
    output = "FAILED test_cache.py::test_restore\ntest_cache.py:65: AssertionError\n" + "noise\n" * 50
    summary = lm.failure_summary(output)
    assert "test_cache.py::test_restore" in summary and "test_cache.py:65" in summary
    assert summary.count("noise") <= 8


def test_native_reflection_invariants_reject_wrong_normal_target():
    import importlib.util
    spec = importlib.util.spec_from_file_location("gate_reflect", os.path.join(ROOT, "tools/shader_reflect/reflect.py"))
    reflect = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reflect)
    key = "shaders/vulkan/mech_tier1.frag/default"
    contract = {"outputs": [{"name": "oGBuffer1", "location": 1, "type": "vec4"}]}
    assert reflect.check_invariants({key: contract}, {}, allow_partial=True) == []
    contract["outputs"][0]["location"] = 2
    assert reflect.check_invariants({key: contract}, {}, allow_partial=True)


def test_deploy_clean_refusal_explains_rebuild(monkeypatch, tmp_path, capsys):
    import deploy_payload as dp
    monkeypatch.setattr(dp, "check_exe_fresh", lambda *a, **k: None)
    monkeypatch.setattr(dp, "_last_failure", [])  # fail() records here; an atexit hook would re-print it after the run
    monkeypatch.setattr(dp, "git_head", lambda *a: "base")
    monkeypatch.setattr(dp, "require_head_exe", lambda *a: {"sha": "base", "dirty": True})
    args = types.SimpleNamespace(allow_stale_exe=False, build64_root=None, require_clean=True)
    with pytest.raises(SystemExit):
        dp.run_deploy(args, str(tmp_path), str(tmp_path), str(tmp_path), "mc2.exe", "mc2.pdb")
    output = capsys.readouterr()
    assert "--require-clean refused deploy" in output.out + output.err
    assert "rebuild" in output.out + output.err
    assert dp.rebuild_command() in output.out + output.err
    assert "scripts/ninja_build.py" in dp.rebuild_command()


@pytest.mark.parametrize("jobs", [1, 6])
def test_contracts_collect_all_red_rows(jobs):
    result = subprocess.run([lm.git_bash(), "scripts/check-contracts.sh", "--selftest-render", f"-j{jobs}"],
                            cwd=ROOT, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert result.returncode == 1
    for label in ("st_fail_talks", "st_fail_chatty", "st_tri_fail"):
        assert label in result.stdout
    assert "WHY THE FAILING ROW(S) FAILED" in result.stdout


def test_gate_dry_run_never_executes(monkeypatch, capsys):
    import merge_gate as gate
    monkeypatch.setattr(gate, "git", lambda *a: (_ for _ in ()).throw(AssertionError("git during execution")))
    def forbidden(*a, **k):
        raise AssertionError("launched subprocess in dry run")
    assert gate.execute(gate.plan(6), "base", True, forbidden) == 0
    assert "validation | DRY-RUN" in capsys.readouterr().out


def test_gate_reports_all_failed_stages(monkeypatch, capsys, tmp_path):
    from contextlib import contextmanager
    import merge_gate as gate
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    @contextmanager
    def batch():
        yield {"MC2_HOST_LEASE_BATCH": "fixture"}
    monkeypatch.setattr(gate, "game_batch", batch)
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    calls = []
    def runner(cmd, **kw):
        calls.append(cmd)
        return _r(2)
    assert gate.execute(gate.plan(6), "head", runner=runner) == 1
    assert len(calls) == len(gate.plan(6))
    assert "deploy | FAIL (2)" in capsys.readouterr().out


@pytest.mark.parametrize("failed", ["contracts", "harnesses"])
def test_gate_offline_failure_prevents_captures_and_deploy(monkeypatch, failed):
    import merge_gate as gate
    calls = []
    def runner(command, **kw):
        calls.append(command[0])
        return _r(int(command[0] == failed))
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "run_child", runner)
    steps = [(name, [name], {}) for name in ("contracts", "harnesses", "capture-before", "deploy")]
    assert gate.execute_parallel(steps, "head") == 1
    assert sorted(calls) == ["contracts", "harnesses"]


def test_gate_refuses_changed_head(monkeypatch):
    import merge_gate as gate
    monkeypatch.setattr(gate, "git", lambda *a: "different")
    assert gate.execute(gate.plan(6), "head", runner=lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran"))) == 1


def test_gate_refuses_real_run_in_lane(monkeypatch):
    import pathlib
    import pytest
    import merge_gate as gate
    # In mc2-main on nifty the guards pass and main([]) would run the REAL gate (contracts -> this
    # pytest -> recursion). Pretend to be a lane worktree and never allow execution.
    monkeypatch.setattr(gate, "ROOT", pathlib.Path("A:/Games/mc2-lane-not-main"))
    monkeypatch.setattr(gate, "execute", lambda *a, **k: pytest.fail("gate executed in a lane"))
    monkeypatch.setattr(gate, "git", lambda *a: "lane-head")
    with pytest.raises(SystemExit) as exc:
        gate.main([])
    assert exc.value.code == 2


@pytest.mark.parametrize("marker", ["1", "0", ""])
def test_gate_recursion_guard_precedes_git_and_execution(monkeypatch, marker):
    import pytest
    import merge_gate as gate
    monkeypatch.setenv("MC2_IN_GATE", marker)
    monkeypatch.setattr(gate, "git", lambda *a: pytest.fail("git reached in nested gate"))
    monkeypatch.setattr(gate, "execute", lambda *a, **k: pytest.fail("nested gate executed"))
    for args in ([], ["--dry-run"]):
        with pytest.raises(SystemExit) as exc:
            gate.main(args)
        assert exc.value.code == 2


def test_gate_children_are_marked(monkeypatch):
    import merge_gate as gate
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    seen = []
    def runner(cmd, **kw):
        seen.append(kw["env"])
        return _r(0)
    assert gate.execute([("test", ["mock-child"], {"CUSTOM": "yes"})], "head", runner=runner) == 0
    assert seen[0]["MC2_IN_GATE"] == "1" and seen[0]["CUSTOM"] == "yes"


def test_gate_stage_selection():
    import pytest
    import merge_gate as gate
    steps = gate.plan(6)
    assert [s[0] for s in gate.select_stages(steps, ["deploy,capture-after"])] == ["deploy", "capture-after"]
    assert "pytest" not in [s[0] for s in gate.select_stages(steps, skip=["pytest"])]
    with pytest.raises(ValueError, match="also needs --skip pytest"):
        gate.select_stages(steps, skip=["contracts"])
    assert "contracts" not in [s[0] for s in gate.select_stages(steps, skip=["contracts,pytest"])]
    for stages, skip in ((["missing"], None), (None, ["missing"]), (["deploy", "deploy"], None),
                         (["deploy"], ["deploy"]), ([], None), (None, list(s[0] for s in steps))):
        with pytest.raises(ValueError):
            gate.select_stages(steps, stages, skip)
    names = [s[0] for s in gate.select_stages(steps)]
    last_proof = max(i for i, n in enumerate(names) if n.startswith(("saveload-", "determinism-")))
    assert names.index("mods") > last_proof


def test_gate_keep_going_runs_past_a_failed_offline_stage(monkeypatch, capsys):
    import merge_gate as gate
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    calls = []
    def runner(command, **kw):
        calls.append(command[0])
        return _r(int(command[0] == "contracts"))
    monkeypatch.setattr(gate, "run_child", runner)
    steps = [("contracts", ["contracts"], {}), ("deploy", ["deploy"], {}), ("tier1", ["stock"], {})]
    assert gate.execute_parallel(steps, "head") == 1
    assert calls == ["contracts"]
    calls.clear()
    assert gate.execute_parallel(steps, "head", keep_going=True) == 1
    assert calls == ["contracts", "deploy", "stock"]


def test_callers_failure_names_exact_allowlist_entry(capsys):
    output = "tracked script has no caller and is not allowlisted: scripts/tests/test_new.py\n"
    assert lm.quick_rows(ROOT, lambda *a: _r(1, output))
    hint = capsys.readouterr().err
    assert 'tests/callers_allowlist.json' in hint
    assert '"scripts/tests/test_new.py": "Pytest-discovered module' in hint


def test_gate_reuses_before_evidence(tmp_path):
    import json
    import merge_gate as gate
    png = tmp_path / "capture.png"
    png.write_bytes(b"png")
    manifest = {"identity": {"exe": {"sha256": "a" * 64}}, "poses": [
        {"bookmark": name, "primary_png": str(png), "all_present": True}
        for name in ("mc2_01_saved_205644", "mc2_10_saved_204523")]}
    (tmp_path / "capture_run.json").write_text(json.dumps(manifest))
    gate.validate_before(tmp_path, "a" * 64)
    for sha in (None, "b" * 64):
        with pytest.raises(ValueError, match="exe|SHA"):
            gate.validate_before(tmp_path, sha)
    steps = gate.plan(6, tmp_path / "proof", before_dir=tmp_path)
    assert "capture-before" not in [s[0] for s in steps]
    compare = next(cmd for name, cmd, _ in steps if name == "compare-captures")
    assert compare[compare.index("--compare-only") + 1] == str(tmp_path)
    png.unlink()
    with pytest.raises(ValueError, match="missing capture"):
        gate.validate_before(tmp_path, "a" * 64)


def test_gate_proofs_dir_routes_log_and_commands(tmp_path, monkeypatch):
    import merge_gate as gate
    with pytest.raises(ValueError, match="no stages selected"):
        gate.execute([], "head", dry_run=True)
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    seen = []
    monkeypatch.setattr(gate, "execute", lambda steps, *a: seen.extend(steps) or 0)
    assert gate.main(["--dry-run", "--proofs-dir", str(tmp_path)]) == 0
    assert (tmp_path / "gate.log").is_file()
    assert str(tmp_path / "before") in next(c for n, c, _ in seen if n == "capture-before")


def test_reused_before_evidence_never_launches_old_classic_exe(tmp_path, monkeypatch):
    import json
    import merge_gate as gate
    before, classic = tmp_path / "before", tmp_path / "before-classic"
    png = tmp_path / "capture.png"
    png.write_bytes(b"png")
    manifest = {"identity": {"exe": {"sha256": "a" * 64}}, "poses": [
        {"bookmark": name, "primary_png": str(png), "all_present": True}
        for name in ("mc2_01_saved_205644", "mc2_10_saved_204523")]}
    for directory in (before, classic):
        directory.mkdir()
        (directory / "capture_run.json").write_text(json.dumps(manifest))
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    seen = []
    monkeypatch.setattr(gate, "execute", lambda steps, *a: seen.extend(steps) or 0)
    args = ["--dry-run", "--before-dir", str(before), "--expect-pre-exe-sha", "a" * 64,
            "--proofs-dir", str(tmp_path / "proof")]
    assert gate.main(args) == 0
    assert not any(n.startswith("capture-before") for n, _, _ in seen)
    compare = next(c for n, c, _ in seen if n == "compare-captures-classic")
    assert compare[compare.index("--compare-only") + 1] == str(classic)
    manifest["identity"]["exe"]["sha256"] = "b" * 64
    (classic / "capture_run.json").write_text(json.dumps(manifest))
    with pytest.raises(SystemExit):
        gate.main(args)


def test_gate_logs_lease_holder_and_age(monkeypatch, capsys):
    import merge_gate as gate
    from smoke_lib import host_lease
    monkeypatch.setattr(host_lease, "status", lambda: {"slots": [
        {"pid": 123, "label": "capture lane", "cwd": "lane", "since": 100}]})
    monkeypatch.setattr(gate.time, "time", lambda: 160)
    gate.log_lease_holders()
    output = capsys.readouterr().out
    assert "pid=123" in output and "held=60s" in output and "capture lane" in output


def test_gate_log_includes_child_stdout_and_stderr(tmp_path, monkeypatch, capsys):
    import merge_gate as gate
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    child = [sys.executable, "-c", "import sys; print('child-out'); print('child-err', file=sys.stderr)"]
    monkeypatch.setattr(gate, "plan", lambda *a: [("child", child, {})])
    execute = gate.execute
    # main is mocked at the execution boundary: it can only run the synthetic child above.
    monkeypatch.setattr(gate, "execute", lambda steps, head, dry: execute(steps, head))
    log = tmp_path / "gate.log"
    assert gate.main(["--dry-run", "--log", str(log)]) == 0
    for output in (log.read_text(), capsys.readouterr().out):
        assert "child-out" in output and "child-err" in output and "| child | PASS" in output


@pytest.mark.parametrize("returncode", [0, 7])
def test_lane_verify_tees_output_and_keeps_failure_tail(tmp_path, capsys, returncode):
    script = tmp_path / "scripts/ninja_build.py"
    script.parent.mkdir()
    script.write_text("import sys\nprint('build-start', flush=True)\nfor n in range(20): print(n)\n"
                      "print('build-out', flush=True)\nprint('build-err', file=sys.stderr)\nsys.exit(" + str(returncode) + ")\n")
    verdict, results = lm.verify_merged(str(tmp_path), "build")
    assert verdict == ("FAIL" if returncode else "OK") and results[0][1] == returncode
    log = (tmp_path / "build64-ninja/lane-merge-verify-build.log").read_text()
    console = capsys.readouterr().out
    assert "build-start" in log and "build-start" in console and "build-start" not in results[0][2]
    for token in ("build-out", "build-err"):
        assert token in log and token in console and token in results[0][2]


def test_batch_resolution_error_explains_next_step(monkeypatch, capsys):
    import pytest
    monkeypatch.setattr(lm, "plan", lambda *a: pytest.fail("merge reached"))
    with pytest.raises(SystemExit) as exc:
        lm.main(["--batch", "one", "two", "--resolution", "path=file"])
    assert exc.value.code == 2
    assert "batch --resolution needs TIP:PATH=FILE" in capsys.readouterr().err


@pytest.mark.parametrize("paths,sim,game", [
    (["docs/intro.md", "tools/lane_merge.py"], False, False),
    (["tools/scifi_kit/art/validate.py"], False, True),
    (["shaders/vulkan/mech.frag"], False, True),
    (["RenderCore/Renderer.h"], True, True),
    (["code/mission.cpp"], True, True),
    (["data/objects/foo.fit"], True, True),
    (["CMakeLists.txt"], True, True),
    (["scripts/generate_gates.py"], True, True),
    (["unknown/file.xyz"], True, True),
    (["tools/foo.py", "code/mission.cpp"], True, True),
    (["scripts/deploy_payload.py"], True, True),
    (["tools/mod_smoke.py"], True, True),
    (["scripts/merge_gate.py"], True, True),
    (["tests/saveload/baselines/mc2_10.json"], True, True),
    (["tests/smoke/sim_determinism/data_overlay.json"], True, True),
    (["tests/contract/default_build_quiet_baseline.json"], True, True),
    (["tools/save_noise_mask.json"], True, True),
    (["scripts/console_child.py"], True, True),
    (["scripts/mc2_env_pin.py"], True, True),
    (["tools/unknown_runtime_helper.py"], True, True),
    (["scripts/tests/test_example.py"], False, False),
])
def test_gate_diff_coverage(paths, sim, game):
    import merge_gate as gate
    steps = gate.affected_plan(gate.plan(6), paths)
    reasons = {name: env.get("MC2_GATE_SKIP_REASON") for name, _, env in steps}
    assert bool(reasons["determinism-mc2_03"]) != sim
    assert bool(reasons["saveload-mc2_01"]) != (sim or "art" in gate.change_domains(paths))
    assert bool(reasons["deploy"]) != game
    assert bool(reasons["mods"]) != game
    assert bool(reasons["capture-after"]) != game
    assert not reasons["contracts"]


def test_full_restores_all_domains_and_reuses_covered_checks():
    import merge_gate as gate
    steps = gate.affected_plan(gate.plan(6), ["docs/intro.md"], full=True)
    skipped = {n for n, _, e in steps if e.get("MC2_GATE_SKIP_REASON")}
    assert skipped == {"pytest", "validation"}
    assert all(e.get("MC2_GATE_REUSE_STAGE") for n, _, e in steps if n in skipped)
    # Selecting a stand-alone stage cannot reuse a check that was never run.
    assert not gate.affected_plan([gate.plan(6)[6]], [], True)[0][2].get("MC2_GATE_SKIP_REASON")


def test_gate_reuse_fails_when_producing_check_failed(monkeypatch, capsys):
    import merge_gate as gate
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    steps = gate.affected_plan([("contracts", ["fake"], {}), ("pytest", ["duplicate"], {})], ["tools/a.py"])
    calls = []
    assert gate.execute(steps, "head", runner=lambda cmd, **kw: calls.append(cmd) or _r(1)) == 1
    assert calls == [["fake"]]
    assert "FAIL reuse" in capsys.readouterr().out


def test_gate_lane_install_routes_every_runtime_command(tmp_path):
    import merge_gate as gate
    steps = gate.install_plan(gate.plan(6), tmp_path)
    for name, cmd, _ in steps:
        assert not any("mc2-win64-v0.4" in arg for arg in cmd), name
    deploy = next(c for n, c, _ in steps if n == "deploy")
    assert str(tmp_path) in deploy and "--allow-new-target" in deploy


def test_tools_and_art_suites_partition_discovery():
    import run_tools_tests as tt
    all_tests = set(tt.discover("all"))
    tools, art = set(tt.discover("tools")), set(tt.discover("art"))
    assert tools and art and not tools & art
    assert tools | art == all_tests


def test_tier1_reuses_only_strict_validated_determinism_smokes():
    import merge_gate as gate
    steps = gate.affected_plan(gate.plan(6), ["code/game.cpp"])
    by_name = {n: (c, e) for n, c, e in steps}
    cmd, _ = by_name["tier1"]
    assert cmd[-8:] == ["--mission", "mc2_01", "--mission", "mc2_03", "--mission", "mc2_17", "--mission", "mc2_24"]
    command, env = by_name["determinism-mc2_10"]
    assert command[-2:] == ["--sim-speed", "2"] and "--headless-both" not in command
    assert env["MC2_VULKAN_VALIDATION"] == env["MC2_MOVER_COHERENCE_AUDIT"] == "1"
    for mission in ("mc2_03", "mc2_24"):   # headless rows: no GPU, so no validation env and no sim-speed
        command, env = by_name["determinism-" + mission]
        assert "--headless-both" in command and "--sim-speed" not in command
        assert "MC2_VULKAN_VALIDATION" not in env
    assert by_name["validation"][1]["MC2_GATE_REUSE_STAGE"] == "tier1,determinism-mc2_10"
    art = gate.affected_plan(gate.plan(6), ["release_assets_src/model.glb"])
    assert "--mission" not in next(c for n, c, _ in art if n == "tier1")
    full = gate.affected_plan(gate.plan(6), ["code/game.cpp"], full=True)
    assert all("--sim-speed" not in c for n, c, _ in full if n.startswith("determinism-") and n != "determinism-mc2_10")   # mc2_10 always runs at 2x (9104ff298)


def test_gate_retains_both_art_modes_for_affected_saves_and_captures():
    import merge_gate as gate
    steps = gate.affected_plan(gate.plan(6), ["mclib/bdactor_type.cpp"])
    names = [n for n, _, _ in steps]
    assert names.index("capture-before-classic") < names.index("deploy")
    assert names.index("capture-after-classic") > names.index("deploy")
    assert "compare-captures-classic" in names
    captures = {n: c for n, c, _ in steps if n.startswith("capture-")}
    assert all("MC2_OPTIONS_OVERRIDE=RemasteredBuildings=" + ("FALSE" if n.endswith("-classic") else "TRUE") in c for n, c in captures.items())
    for mission in ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24"):
        modes = [(c, e) for n, c, e in steps if n.startswith("saveload-" + mission)]
        assert len(modes) == 2 and modes[0][0] != modes[1][0]
        assert {e["MC2_OPTIONS_OVERRIDE"] for _, e in modes} == {"RemasteredBuildings=TRUE", "RemasteredBuildings=FALSE"}
        assert all("--sim-speed" not in c for c, _ in modes)
        assert modes[1][0][-2:] == ["--duration", "20"]
        assert ("--duration" in modes[0][0]) == (mission not in ("mc2_10", "mc2_24"))
    full = gate.affected_plan(gate.plan(6), ["scripts/merge_gate.py"], full=True)
    assert all("--sim-speed" not in c and "--duration" not in c
               for n, c, _ in full if n.startswith("saveload-"))


@pytest.mark.parametrize("paths,count", [(["code/mission.cpp"], 2), (["scripts/kit_payload.py"], 10),
                                        (["tools/scifi_kit/art/intent.py"], 10), (["unknown/runtime.xyz"], 10),
                                        (["mclib/content_ir.cpp"], 10)])
def test_gate_save_matrix_tracks_art_path(paths, count):
    import merge_gate as gate
    cases = [(n, e) for n, _, e in gate.affected_plan(gate.plan(6), paths)
             if n.startswith("saveload-") and not e.get("MC2_GATE_SKIP_REASON")]
    assert len(cases) == count
    if count == 2:
        assert {n for n, _ in cases} == {"saveload-mc2_01", "saveload-mc2_10"}


def test_captures_keep_symmetric_env_and_do_not_mutate_gameplay(tmp_path):
    import merge_gate as gate
    steps = gate.install_plan(gate.affected_plan(gate.plan(6, tmp_path),
        ["scripts/kit_payload.py"]), tmp_path / "install")
    by_name = {n: (c, e) for n, c, e in steps}
    for suffix in ("", "-classic"):
        before, before_env = by_name["capture-before" + suffix]
        after, after_env = by_name["capture-after" + suffix]
        before = list(before)
        before[before.index("--out") + 1] = after[after.index("--out") + 1]
        assert before == after and before_env == after_env
        assert not after_env.get("MC2_GATE_SKIP_REASON")
    command, env = by_name["tier1"]
    assert "--capture-bookmark" not in command and "--quiet-sample" not in command
    assert command[command.index("--mission"):] == ["--mission", "mc2_01", "--mission", "mc2_03", "--mission", "mc2_17", "--mission", "mc2_24"]
    assert env["MC2_VULKAN_VALIDATION"] == "1"
    assert "--fresh-smoke" in by_name["quiet"][0]


@pytest.mark.parametrize("failure", [False, True])
def test_parallel_gate_finishes_mods_before_stock_and_keeps_deploy_barrier(monkeypatch, failure):
    import threading
    import merge_gate as gate
    started, stock_done = threading.Event(), threading.Event()
    calls = []
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    def runner(command, **kw):
        calls.append(command[0])
        assert "MC2_HOST_LEASE_BATCH" not in kw["env"]
        if command == ["mods"]:
            assert "deploy" in calls
            started.set()
            assert not stock_done.wait(0.05), "stock I/O overlaps cold mod load"
            return _r(1 if failure else 0)
        if command == ["stock"]:
            assert started.is_set()
            stock_done.set()
        return _r(0)
    monkeypatch.setattr(gate, "run_child", runner)
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    assert gate.execute_parallel([("deploy", ["deploy"], {}), ("mods", ["mods"], {}),
                                  ("tier1", ["stock"], {})], "head") == int(failure)
    assert started.is_set() and stock_done.is_set()


@pytest.mark.parametrize("failed_source", [None, "tier1", "det", "missing"])
def test_parallel_validation_reuse_joins_both_workers(tmp_path, monkeypatch, capsys, failed_source):
    import threading
    from contextlib import contextmanager
    import merge_gate as gate
    tier_done = threading.Event()
    det_done = threading.Event()
    @contextmanager
    def copy(source):
        yield tmp_path / "clone"
    def runner(command, **kw):
        name = command[0]
        if name == "tier1":
            assert det_done.wait(2)
            tier_done.set()
        elif name == "det":
            det_done.set()
            assert tier_done.wait(2)
        else:
            assert name == "mods", "validation must reuse evidence, not launch again"
        return _r(int(name == failed_source))
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "stock_copy", copy)
    monkeypatch.setattr(gate, "run_child", runner)
    sources = "tier1,determinism-mc2_10" + (",missing" if failed_source == "missing" else "")
    steps = [("mods", ["mods"], {}),
             ("tier1", ["tier1"], {}),
             ("validation", ["validation"], {"MC2_GATE_SKIP_REASON": "covered", "MC2_GATE_REUSE_STAGE": sources}),
             ("determinism-mc2_10", ["det", "--install", str(tmp_path / "install")], {})]
    assert gate.execute_parallel(steps, "head") == int(failed_source is not None)
    output = capsys.readouterr().out
    assert ("FAIL reuse" in output) == (failed_source is not None)
    assert ("validation: REUSED" in output) == (failed_source is None)


def test_smoke_bookmark_capture_rejects_missing_or_wrong_evidence(tmp_path, monkeypatch):
    import json
    import run_visual_capture as capture
    bookmark = tmp_path / "day.json"
    bookmark.write_text(json.dumps({"mission": "mc2_01", "bookmarks": [{"name": "one"}, {"name": "two"}]}))
    out = tmp_path / "out"
    assert capture.smoke_capture([bookmark], "mc2_17", out) == {}
    env = capture.smoke_capture([bookmark], "mc2_01", out)
    assert env["MC2_VISUAL_CAPTURE_FRAME"] == "431" and env["MC2_VISUAL_CAPTURE_CLEAN"] == "1"
    assert env["MC2_SMOKE_FIXED_TIMESTEP"] == "1"
    with pytest.raises(RuntimeError, match="missing"):
        capture.smoke_capture([bookmark], "mc2_01", out, True)


    for name in ("one", "two"):
        png = out / "day" / f"mc2_01_{name}.png"
        png.write_bytes(b"png")
        png.with_suffix(".json").write_text(json.dumps({"deterministic": True, "backend": "vulkan",
                                                        "render_options": {"RemasteredBuildings": True}}))
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"fixture exe")
    monkeypatch.setattr(capture.ms, "identity_block", lambda **kw: {"exe": {"sha256": capture.ms.file_sha256(exe)}})
    capture.smoke_capture([bookmark], "mc2_01", out, True, exe=exe)
    manifest = json.loads((out / "capture_run.json").read_text())
    assert manifest["n_captured"] == manifest["n_requested"] == 1
    assert manifest["identity"]["exe"]["sha256"] == capture.ms.file_sha256(exe)
    png.with_suffix(".json").write_text('{}')
    with pytest.raises(RuntimeError, match="invalid"):
        capture.smoke_capture([bookmark], "mc2_01", out, True)


def test_parallel_gate_respects_inherited_batch_and_failed_deploy(monkeypatch):
    import merge_gate as gate
    seen = []
    monkeypatch.setenv("MC2_HOST_LEASE_BATCH", "parent")
    monkeypatch.setattr(gate, "execute", lambda steps, *a, **kw: seen.extend(steps) or 1)
    steps = [("deploy", ["deploy"], {}), ("mods", ["mods"], {})]
    assert gate.execute_parallel(steps, "head") == 1 and seen == steps
    seen.clear()
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH")
    assert gate.execute_parallel(steps, "head") == 1 and seen == steps[:1]


@pytest.mark.parametrize("fail", [False, True])
def test_stock_copy_is_private_and_always_removed(tmp_path, fail):
    import merge_gate as gate
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stock-copy-test-", dir=gate.ROOT / "scratch") as work:
        tmp_path = gate.Path(work)
        install = tmp_path / "install"
        (install / "data").mkdir(parents=True)
        (install / "data/asset").write_text("source")
        (install / "mc2.exe").write_text("engine")
        (install / "cache").mkdir()
        (install / "cache/model.mci").write_text("warm cache")
        native = install / "cache/content-model/model-0000000000000001.mci"
        native.parent.mkdir()
        native.write_bytes(b"native")
        receipt = native.parent / "industrial-cook.json"
        receipt.write_text("{}")
        (install / "mod_smoke-stage").mkdir()
        (install / "mod_smoke-stage/large").write_text("disposable")
        try:
            with gate.stock_copy(install) as clone:
                assert clone.parent == install.parent
                assert not (clone / "mod_smoke-stage").exists()
                assert (clone / "cache/model.mci").read_text() == "warm cache"
                recipe = clone / native.relative_to(install)
                assert recipe.samefile(native)
                assert not (clone / receipt.relative_to(install)).samefile(receipt)
                replacement = recipe.with_suffix(".mci.tmp")
                replacement.write_bytes(b"updated")
                replacement.replace(recipe)
                assert native.read_bytes() == b"native"
                assert not list(clone.glob(".gate-stock-*"))
                assert (clone / "data/asset").samefile(install / "data/asset")
                (clone / "mc2.exe").write_text("changed")
                assert (install / "mc2.exe").read_text() == "engine"
                if fail:
                    raise RuntimeError("fixture")
        except RuntimeError:
            assert fail
        assert not clone.exists()


def test_offline_stages_overlap_before_deploy(monkeypatch):
    import threading
    import merge_gate as gate
    harness_started = threading.Event()
    calls = []
    def runner(command, **kw):
        if command == ["harness"]:
            harness_started.set()
        if command == ["contracts"]:
            assert harness_started.wait(2)
        calls.append(command[0])
        return _r(0)
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "run_child", runner)
    assert gate.execute_parallel([("contracts", ["contracts"], {}), ("harnesses", ["harness"], {}),
                                  ("deploy", ["deploy"], {})], "head") == 0
    assert calls[-1] == "deploy" and set(calls) == {"contracts", "harness", "deploy"}


@pytest.mark.parametrize('with_determinism', [True, False])
def test_auxiliary_determinism_uses_copy_while_saves_stay_serial(tmp_path, monkeypatch, with_determinism):
    import threading
    from contextlib import contextmanager
    import merge_gate as gate
    install, clone = tmp_path / "install", tmp_path / "clone"
    saves, det, saving, mods_done = threading.Event(), threading.Event(), threading.Event(), threading.Event()
    copying = threading.Event()
    calls = []
    @contextmanager
    def copy(source):
        assert source == install.resolve()
        # Keep the snapshot open long enough to expose premature runtime launch.
        assert not saves.wait(0.05)
        calls.append("copy-complete")
        copying.set()
        yield clone
        calls.append("cleaned")
    def runner(command, **kw):
        calls.append(command[0])
        if command[0] != "deploy":
            assert "copy-complete" in calls
        if command[0] == "mods":
            assert copying.wait(2)
            assert command[command.index("--base-install") + 1] == str(clone)
            assert command[command.index("--exe") + 1] == str(clone / "mc2.exe")
            mods_done.set()
        if command[0] == "det":
            assert command[-1] == str(clone)
            # W83: a concurrent mod cold load starved this fixture of frames.
            assert mods_done.is_set()
            det.set()
        if command[0] == "save":
            # Neither proof may share a window with the mods battery (W83).
            assert mods_done.is_set() and not saving.is_set()
            saving.set()
            saves.set()
        if command[0] == "kit":
            assert command[-1] == str(clone)
        if command[0] == "quiet":
            assert command[-1] == str(clone / "mc2.exe")
        return _r(0)
    monkeypatch.delenv("MC2_HOST_LEASE_BATCH", raising=False)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "stock_copy", copy)
    monkeypatch.setattr(gate, "run_child", runner)
    steps = [("deploy", ["deploy"], {}),
        ("mods", ["mods", "--base-install", str(install), "--exe", str(install / "mc2.exe")], {}),
        ("determinism-mc2_10", ["det", "--install", str(install)], {}),
        ("saveload-mc2_01", ["save", "--install", str(install)], {}),
        ("kit-visibility", ["kit", "--install", str(install)], {}),
        ("quiet", ["quiet", "--exe", str(install / "mc2.exe")], {})]
    if not with_determinism:
        steps = [s for s in steps if s[0] not in ('determinism-mc2_10', 'kit-visibility', 'quiet')]
    assert gate.execute_parallel(steps, "head") == 0
    assert calls.index("mods") < calls.index("save") < calls.index("cleaned")
    if with_determinism:
        assert calls.index("mods") < calls.index("det") < calls.index("kit") < calls.index("quiet") < calls.index("save")


@pytest.mark.parametrize("tier,env,expected", [
    ("tier1", {"MC2_WIDTH": "1920", "MC2_HEIGHT": "1080"}, (1920, 1080)),
    ("tier1", {"MC2_WIDTH": "854", "MC2_HEIGHT": "480"}, (854, 480)),
    ("tier1", {}, (1920, 1080)), ("adhoc", {}, None),
])
def test_resolution_proof_uses_effective_capture_size_and_keeps_unpinned_window(tier, env, expected):
    from smoke_lib import render_resolution
    assert render_resolution.asked_size(tier, effective_env=env) == expected


@pytest.mark.parametrize("generator,failure", [("Ninja", ""), ("Visual Studio", ""), ("missing", ""),
                                               ("Ninja-fallback", ""), ("Ninja-fallback", "build"), ("Ninja-fallback", "native")])
def test_unit_runner_engine_cache_layout_and_required_coverage(tmp_path, generator, failure):
    from pathlib import Path
    root = tmp_path / "repo"
    (root / "scripts").mkdir(parents=True)
    script = root / "scripts/run-unit-tests.sh"
    script.write_bytes(Path(ROOT, "scripts/run-unit-tests.sh").read_bytes())
    engine = "build64-ninja" if generator == "Ninja" else "build64"
    (root / engine).mkdir()
    (root / engine / "CMakeCache.txt").write_bytes(("CMAKE_GENERATOR:INTERNAL=" + generator.replace('-fallback', '') + "\r\n").encode())
    cfg = "" if generator.startswith("Ninja") else "RelWithDebInfo/"
    outputs = ["build64-tests/RelWithDebInfo/mc2_tests.exe", "build64-tests/RelWithDebInfo/test_render_contract_3.exe",
               engine + "/tests/model_override/" + cfg + "model_override_tests.exe",
               engine + "/tests/static_natural_skip/" + cfg + "static_natural_skip_policy_tests.exe"]
    (root / "scripts/ninja_build.py").write_text(
        "from pathlib import Path\nimport os, sys\nroot = Path(__file__).parents[1]\n"
        "tree = Path(os.environ.get('MC2_NINJA_TREE', 'build64-ninja'))\n"
        "assert not tree.is_absolute(), 'ninja_build requires a repo-relative tree'\n"
        "if not tree.is_absolute(): tree = root / tree\n"
        "root.joinpath('ninja-calls').write_text(' '.join(sys.argv[1:]))\n"
        + ("sys.exit(9)\n" if failure == 'build' else '')
        + "for folder, name in [('model_override', 'model_override_tests'), ('static_natural_skip', 'static_natural_skip_policy_tests')]:\n"
        "    exe = tree / 'tests' / folder / (name + '.exe')\n"
        "    exe.parent.mkdir(parents=True, exist_ok=True)\n"
        f"    exe.write_text('#!/usr/bin/env bash\\nexit {7 if failure == 'native' else 0}\\n', newline='\\n')\n")
    # A wrong-tree build leaves these failing stale binaries behind.
    for rel in outputs[2:]:
        exe = root / rel
        exe.parent.mkdir(parents=True, exist_ok=True)
        exe.write_text('#!/usr/bin/env bash\nexit 8\n', newline='\n')
    fake = root / "cmake.sh"
    fake.write_text('#!/usr/bin/env bash\nset -eu\nhere="$(cd "$(dirname "$0")" && pwd)"\n'
                    'echo "$*" >> "$here/cmake-calls"\nfor rel in ' + " ".join(outputs[:2] if generator.startswith('Ninja') else outputs) + '; do\n'
                    'mkdir -p "$(dirname "$here/$rel")"\nprintf "#!/usr/bin/env bash\\nexit 0\\n" > "$here/$rel"\n'
                    'chmod +x "$here/$rel"\ndone\n', encoding="utf-8", newline="\n")
    fake.chmod(0o755)
    env = dict(os.environ, MC2_CMAKE=fake.as_posix(), MC2_BUILD_JOBS="1", MC2_UNIT_BUILD_DIR=(root / "build64-tests").as_posix())
    env.pop("MC2_NINJA_TREE", None)
    if generator == "Ninja":
        env["MC2_NINJA_TREE"] = "build64-ninja"
    elif generator == "missing":
        env["MC2_NINJA_TREE"] = "absent-engine"
    result = subprocess.run([lm.git_bash(), script.as_posix()], cwd=root, env=env,
                            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if generator == "missing":
        assert result.returncode != 0 and "requested engine cache is missing" in result.stdout
    elif failure:
        assert result.returncode != 0, result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        assert "run model_override_tests" in result.stdout
        assert "run static_natural_skip_policy_tests" in result.stdout
        assert "SKIP" not in result.stdout
        if generator.startswith("Ninja"):
            assert "model_override_tests static_natural_skip_policy_tests" in (root / "ninja-calls").read_text()


@pytest.mark.parametrize("fail", [False, True])
def test_game_batch_releases_and_reaps_its_holder(monkeypatch, fail):
    import merge_gate as gate
    import psutil
    from smoke_lib import host_lease
    monkeypatch.delenv(host_lease.BATCH_ENV, raising=False)
    calls = []
    monkeypatch.setattr(host_lease, "hold", lambda tag, **kw: calls.append(("hold", tag, kw)) or 123)
    monkeypatch.setattr(host_lease, "release_batch", lambda tag: calls.append(("release", tag)))
    monkeypatch.setattr(psutil, "Process", lambda pid: types.SimpleNamespace(wait=lambda **kw: calls.append(("wait", pid))))
    try:
        with gate.game_batch() as env:
            assert env[host_lease.BATCH_ENV] == calls[0][1]
            assert calls[0][2]["exclusive"] is True  # captures require all host slots
            if fail:
                raise RuntimeError("fixture failed")
    except RuntimeError as exc:
        assert fail and str(exc) == "fixture failed"
    assert [c[0] for c in calls] == ["hold", "release", "wait"]


def test_only_capture_stages_take_the_exclusive_hold_one_at_a_time(monkeypatch):
    from contextlib import contextmanager
    import merge_gate as gate
    calls = []
    @contextmanager
    def batch():
        calls.append("hold")
        try:
            yield {"MC2_HOST_LEASE_BATCH": "fixture"}
        finally:
            calls.append("release")
    def runner(cmd, **kw):
        calls.append(cmd[0])
        assert ("MC2_HOST_LEASE_BATCH" in kw["env"]) == (cmd[0] == "before")
        return _r(0)
    monkeypatch.setattr(gate, "run_child", runner)
    monkeypatch.setattr(gate, "game_batch", batch)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    steps = [("contracts", ["offline"], {}), ("capture-before", ["before"], {}), ("deploy", ["deploy"], {}), ("tier1", ["smoke"], {})]
    assert gate.execute(steps, "head", runner=runner) == 0
    assert calls == ["offline", "hold", "before", "release", "deploy", "smoke"]


@pytest.mark.parametrize("fresh", [True, False])
def test_gate_reuses_only_fresh_same_head_unit_receipt(tmp_path, monkeypatch, fresh):
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    import json
    import time
    import merge_gate as gate
    (tmp_path / "build64-ninja").mkdir()
    (tmp_path / "build64-ninja/merge-window.json").write_text(json.dumps(
        dict(head="head", base="base", verify="OK", started=time.time() - (0 if fresh else 3600))))
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    seen = []
    monkeypatch.setattr(gate, "execute", lambda steps, *a: seen.extend(steps) or 0)
    assert gate.main(["--dry-run", "--proofs-dir", str(tmp_path / "proof")]) == 0
    cmd = next(c for n, c, _ in seen if n == "harnesses")
    assert ("--python-only" in cmd) == fresh


def test_art_selection_handles_git_quoted_unicode_paths(tmp_path, monkeypatch):
    import run_tools_tests as tt
    root = _throwaway_repo(tmp_path)
    base = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True,
                                   **console_child.child_kwargs(redirected=True)).strip()
    asset = root / "data/kit/caf\u00e9.glb"
    asset.parent.mkdir(parents=True)
    asset.write_bytes(b"fixture")
    for args in (["add", "--", str(asset)], ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "asset"]):
        subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True,
                       **console_child.child_kwargs(redirected=True))
    monkeypatch.setattr(tt, "ROOT", str(root))
    assert tt.art_changed(base)


def test_gate_without_window_receipt_keeps_full_both_mode_coverage(tmp_path, monkeypatch):
    import merge_gate as gate
    monkeypatch.delenv("MC2_IN_GATE", raising=False)
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    seen = []
    monkeypatch.setattr(gate, "execute", lambda steps, *a: seen.extend(steps) or 0)
    assert gate.main(["--dry-run", "--proofs-dir", str(tmp_path / "proof")]) == 0
    assert len([n for n, _, _ in seen if n.startswith("saveload-")]) == 10
    assert "capture-before-classic" in {n for n, _, _ in seen}
    assert all("MC2_GATE_SKIP_REASON" not in env for name, _, env in seen
               if name.startswith(("capture-", "saveload-", "determinism-")))
    assert all(e["MC2_SMOKE_ARTIFACT_ROOT"] == str(tmp_path / "proof/smokes" / n)
               for n, _, e in seen if n == "mods" or n.startswith(("determinism-", "saveload-")))


def test_blocked_capture_reports_missing_proof_without_launching(monkeypatch, capsys):
    from contextlib import contextmanager
    import merge_gate as gate
    @contextmanager
    def unavailable():
        raise RuntimeError("fixture: FIFO queue busy")
        yield {}
    def runner(cmd, **k):
        if cmd[0] == "capture":
            pytest.fail("blocked stage must not launch")
        return _r(0)
    monkeypatch.setattr(gate, "game_batch", unavailable)
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    steps=[("capture-before", ["capture"], {}), ("tier1", ["smoke"], {"MC2_GATE_COVERAGE": "missions assigned to shared proofs"})]
    assert gate.execute(steps, "head", runner=runner) == 1
    output=capsys.readouterr().out
    assert "| capture-before | FAIL: fixture: FIFO queue busy" in output
    assert "| tier1 | PASS" in output
    assert "Coverage plan tier1:" in output


def test_pre_gate_runs_every_step_and_prints_one_verdict(monkeypatch, capsys, tmp_path):
    exe = tmp_path / "mc2.exe"
    exe.write_text("x")
    monkeypatch.setattr(lm, "regenerate_all", lambda root, dry_run=False: [])
    ran = []

    def fake_run(root, cmd, env=None):
        ran.append(os.path.basename(cmd[2]))
        return _r(1 if "gate_gen.py" in cmd[2] else 0, "stale table")
    monkeypatch.setattr(lm, "run", fake_run)
    assert lm.pre_gate(str(tmp_path), str(exe)) == 1
    out = capsys.readouterr().out
    assert ran == ["check-background-launch.py", "gate_gen.py", "cxx17_ratchet.py", "run_smoke.py"]  # a red step does not stop the rest
    assert out.strip().splitlines()[-1] == "PRE-GATE FAIL: gate_gen"
    monkeypatch.setattr(lm, "run", lambda root, cmd, env=None: _r(0))
    assert lm.pre_gate(str(tmp_path), str(tmp_path / "none.exe")) == 0
    assert "PRE-GATE PASS (headless-smoke skipped: no exe)" in capsys.readouterr().out


def test_ninja_build_warns_on_dirty_tree_and_stale_exe(monkeypatch, tmp_path):
    import ninja_build as nb
    import exe_fingerprint as fpmod
    calls = {"status": " M a.cpp\n", "rev-parse": "b" * 40 + "\n"}
    monkeypatch.setattr(nb.subprocess, "run", lambda cmd, **k: types.SimpleNamespace(stdout=calls[cmd[3]]))
    (tmp_path / "t").mkdir()
    (tmp_path / "t/mc2.exe").write_bytes(b"x")
    monkeypatch.setattr(fpmod, "extract", lambda p: {"present": True, "sha": "a" * 12})
    line = nb.build_warning(str(tmp_path), "t")
    assert line.startswith("[ninja_build] WARNING:") and "1 uncommitted" in line and "newer than the last build" in line
    assert "\n" not in line
    calls["status"] = ""
    monkeypatch.setattr(fpmod, "extract", lambda p: {"present": True, "sha": "b" * 12})
    assert nb.build_warning(str(tmp_path), "t") == ""
