"""Tests for the merge-window speedups: lane_merge quick rows before the verify, ninja_build oversize-pdb guard (no game, no build)."""
import os
import sys
import types

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
        subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=t", "-c", "user.email=t@t", *a], check=True, capture_output=True)
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
    monkeypatch.setattr(lm, "compiled_inputs_changed", lambda *a: True)
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
