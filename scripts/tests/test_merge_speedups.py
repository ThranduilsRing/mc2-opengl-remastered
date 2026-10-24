"""Tests for the merge-window speedups: lane_merge quick rows before the verify, ninja_build oversize-pdb guard (no game, no build)."""
import os
import sys
import types
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
    pdb.write_bytes(b"x" * 100)
    assert not ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=1000) and pdb.exists()
    assert ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=50)
    assert not pdb.exists() and "deleted t/mc2.pdb" in capsys.readouterr().out
    assert not ninja_build.drop_oversize_pdb(str(tmp_path), "t", limit=50)  # missing pdb: no error


def test_tooling_only_regex_covers_build_inputs():
    for p in ("a/res.rc", "a/x.def", "data/x.ini", "cfg.h.in", "cmake/Find.txt", "scripts/generate_gates.py", "x/CMakeLists.txt"):
        assert lm.COMPILED_INPUTS.search(p), p
    for p in ("tools/lane_merge.py", "docs/a.md", "scripts/tests/test_x.py"):
        assert not lm.COMPILED_INPUTS.search(p), p


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
    assert lm.verify_env(ROOT)["MC2_NINJA_TREE"] == "build64-ninja" and lm.verify_env(ROOT)["MC2_UNIT_BUILD_DIR"] == os.path.join(ROOT, "build64-tests")


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
    assert len(steps) == 15
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


def test_gate_dry_run_never_executes(monkeypatch, capsys):
    import merge_gate as gate
    monkeypatch.setattr(gate, "git", lambda *a: (_ for _ in ()).throw(AssertionError("git during execution")))
    def forbidden(*a, **k):
        raise AssertionError("launched subprocess in dry run")
    assert gate.execute(gate.plan(6), "base", True, forbidden) == 0
    assert "validation | DRY-RUN" in capsys.readouterr().out


def test_gate_red_stops_remaining_stages(monkeypatch, capsys):
    import merge_gate as gate
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    calls = []
    def runner(cmd, **kw):
        calls.append(cmd)
        return _r(2)
    assert gate.execute(gate.plan(6), "head", runner=runner) == 1
    assert len(calls) == 1
    assert "deploy | SKIPPED" in capsys.readouterr().out


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
    assert [s[0] for s in gate.select_stages(steps, ["deploy,capture-after", "contracts"], ["contracts"])] == ["deploy", "capture-after"]
    assert "pytest" not in [s[0] for s in gate.select_stages(steps, skip=["pytest"])]
    for stages, skip in ((["missing"], None), (None, ["missing"]), (["deploy", "deploy"], None)):
        with pytest.raises(ValueError):
            gate.select_stages(steps, stages, skip)


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
    assert "merge that tip alone with --resolution PATH=FILE, then run --batch" in capsys.readouterr().err
