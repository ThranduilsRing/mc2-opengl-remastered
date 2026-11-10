"""--lint: the precheck's fast checks, concurrently, with a fix hint per red (no game, no build)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from tools import lane_merge as lm  # noqa: E402


def test_lint_rows_share_precheck_tables():
    rows = lm.lint_rows(["py"], "base", "head")
    labels = [r[0] for r in rows]
    for label, _s, _a in lm.RATCHETS + lm.EARLY_ROWS:
        assert label in labels
    assert "callers" in labels and "ungated_tags" in labels
    assert "background_launch" in labels and "new_drive_paths" in labels   # W138d: the merge gate caught both after lint passed
    assert not any("--tighten" in r[1] for r in rows)


def _no_branch_needed(monkeypatch):
    """A shared clone in the precheck has no claude/nifty-mendeleev branch: resolve every ref to HEAD's sha."""
    real = lm.git_out
    monkeypatch.setattr(lm, "git_out", lambda root, *a: real(root, "rev-parse", "HEAD") if a[:1] == ("rev-parse",) else real(root, *a))


def test_lint_reports_red_with_fix_and_exit_code(tmp_path, capsys, monkeypatch):
    _no_branch_needed(monkeypatch)
    def runner(root, cmd):
        bad = "callers.py" in " ".join(cmd) and "--check" in cmd
        return subprocess.CompletedProcess(cmd, 1 if bad else 0, stdout="boom" if bad else "", stderr="")
    rc = lm.lint(ROOT, "HEAD", runner=runner)
    out = capsys.readouterr().out
    assert rc == 1
    assert "lint: callers: FAIL" in out and "fix:" in out
    assert "lint: cxx17: PASS" in out
    assert "inventories" not in out   # proof-diet: lint never regenerates inventories


def test_lint_green(capsys, monkeypatch):
    _no_branch_needed(monkeypatch)
    ok = lambda root, cmd: subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
    assert lm.lint(ROOT, "HEAD", runner=ok) == 0
    assert "LINT PASS" in capsys.readouterr().out


def test_batch_lint_ok_follows_lint_rc(monkeypatch):
    seen = []
    assert lm.batch_lint_ok("r", lambda root, ref: seen.append((root, ref)) or 0)
    assert not lm.batch_lint_ok("r", lambda root, ref: 1)
    assert seen == [("r", "HEAD")]
    text = open(lm.__file__, encoding="utf-8").read()
    assert "args.stage and not batch_lint_ok(root)" in text
