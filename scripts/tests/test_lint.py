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
    rc = lm.lint(ROOT, "HEAD", runner=runner, regen=lambda r, c: ["tests/callers.json"])
    out = capsys.readouterr().out
    assert rc == 1
    assert "lint: callers: FAIL" in out and "fix:" in out
    assert "inventories: FAIL" in out and "--regenerate-all" in out
    assert "lint: file_size: PASS" in out


def test_lint_green(capsys, monkeypatch):
    _no_branch_needed(monkeypatch)
    ok = lambda root, cmd: subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
    assert lm.lint(ROOT, "HEAD", runner=ok, regen=lambda r, c: []) == 0
    assert "LINT PASS" in capsys.readouterr().out
