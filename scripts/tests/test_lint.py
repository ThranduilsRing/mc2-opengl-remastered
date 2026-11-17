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
    monkeypatch.setenv("GIT_LFS_SKIP_SMUDGE", "1")  # mocked lint rows never consume Windows prebuilt dependencies
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


def test_generated_hint_names_the_regen_command():
    tail = ("split_map: FAIL - docs/split-map.md is stale (a split piece was added, removed or re-described); "
            "run: py -3 tools/split/split_map.py --write")
    assert lm.generated_hint(tail) == ("only generated files are stale (docs/split-map.md); "
                                       "the merge regenerates them, or run py -3 tools/split/split_map.py --write")


def test_generated_hint_background_launch_inventory():
    assert lm.generated_hint("docs/background-launch-inventory.md is stale") == (
        "only generated files are stale (docs/background-launch-inventory.md); "
        "the merge regenerates them, or run py -3 scripts/check-background-launch.py --write --quiet")


def test_generated_hint_suppressed_when_other_files_fail():
    assert lm.generated_hint("code/foo.cpp:12 [LG08] violation") is None
    assert lm.generated_hint("docs/split-map.md is stale; code/foo.cpp:12 also fails") is None


def test_report_lint_row_prints_generated_hint(capsys):
    lm.report_lint_row("split_map", 1, 0.4,
                       "split_map: FAIL - docs/split-map.md is stale; run: py -3 tools/split/split_map.py --write",
                       "fix the code, or lower the baseline via the ratchet's own --tighten (never raise it)")
    out = capsys.readouterr().out
    assert "lint: split_map: FAIL" in out
    assert "fix: fix the code" in out
    assert ("only generated files are stale (docs/split-map.md); "
            "the merge regenerates them, or run py -3 tools/split/split_map.py --write") in out


def test_report_lint_row_pass_prints_no_hint(capsys):
    lm.report_lint_row("cxx17", 0, 1.2, "", "fix the code")
    assert capsys.readouterr().out == "lint: cxx17: PASS (1s)\n"
