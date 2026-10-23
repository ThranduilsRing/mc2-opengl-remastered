"""Tests for the merge-window speedups: lane_merge quick rows before the verify, ninja_build oversize-pdb guard (no game, no build)."""
import os
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
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


def test_batch_stops_before_verify_when_quick_rows_red(monkeypatch):
    verifies = []
    monkeypatch.setattr(lm, "quick_rows", lambda root, runner=None: [("script_callers", 1, "bad")])
    monkeypatch.setattr(lm, "verify_merged", lambda *a, **k: verifies.append(a) or ("OK", []))
    src = open(os.path.join(ROOT, "tools", "lane_merge.py"), encoding="utf-8").read()
    # the gate sits between the merge loop and verify_merged, and exits 6 like the ratchet stop
    i, j, k = src.index("qfailed = quick_rows(root)"), src.index("BATCH STOPPED at quick rows"), src.index("verify_merged(root, mode)")
    assert i < j < k and "return 6" in src[j:k]


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
    assert subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True).stdout.strip() == "''"
    env["MC2_COMMIT_TRAILER"] = "Co-Authored-By: X <x@y>"
    assert "Co-Authored-By: X" in subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True).stdout
