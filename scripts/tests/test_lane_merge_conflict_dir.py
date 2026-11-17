"""lane_merge --batch --dry-run --conflict-dir: per-tip conflict files and summaries from a temp repo (no game, no build)."""
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
import console_child  # noqa: E402


def test_overlapping_receipt_writers_do_not_share_temps(tmp_path, monkeypatch):
    import json
    sha = "a" * 40
    monkeypatch.setattr(lm, "PRECHECK_RECEIPTS", str(tmp_path))
    monkeypatch.setattr(lm, "git_out", lambda *args: sha)
    replace = os.replace
    pending = []
    def overlapping(src, dst):
        pending.append(str(src))
        if len(pending) == 1:
            lm.write_receipt("unused", sha, False, "PASS")
        replace(src, dst)
    monkeypatch.setattr(os, "replace", overlapping)
    lm.write_receipt("unused", sha, True, "PASS")
    assert len(set(pending)) == 2
    assert json.loads((tmp_path / (sha + ".json")).read_text())["link"] is True
    assert len(list(tmp_path.iterdir())) == 1


@pytest.fixture(autouse=True)
def _in_process(monkeypatch):
    monkeypatch.setenv("MC2_LANE_MERGE_TRUSTED", "1")


def git(repo, *args):
    r = subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", *args],
                       capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def commit(repo, branch, base, files, msg):
    git(repo, "checkout", "-q", "-B", branch, base)
    for name, text in files.items():
        p = repo / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, newline="\n")
        git(repo, "add", name)
    git(repo, "commit", "-q", "-m", msg)
    return git(repo, "rev-parse", "HEAD")


BASE = "".join(f"line {i}\n" for i in range(1, 11))


@pytest.fixture
def repo(tmp_path):
    r = tmp_path / "repo"
    r.mkdir()
    git(r, "init", "-q", "-b", "main")
    git(r, "config", "user.name", "t")
    git(r, "config", "user.email", "t@t")
    (r / "a.txt").write_text(BASE, newline="\n")
    (r / "b.txt").write_text(BASE, newline="\n")
    git(r, "add", ".")
    git(r, "commit", "-q", "-m", "base")
    return r


def run(repo, out, *tips, extra=()):
    git(repo, "checkout", "-q", "main")
    return lm.main(["--root", str(repo), "--batch", *tips, "--dry-run", "--conflict-dir", str(out), *extra])


def test_conflict_files_and_clean_tip(repo, tmp_path, capsys):
    t1 = commit(repo, "t1", "main", {"a.txt": BASE.replace("line 5", "ONE")}, "t1")
    t2 = commit(repo, "t2", "main", {"a.txt": BASE.replace("line 5", "TWO")}, "t2")
    t3 = commit(repo, "t3", "main", {"c.txt": "new\n"}, "t3")
    head = git(repo, "rev-parse", "main")
    out = tmp_path / "out"
    assert run(repo, out, t1, t2, t3) == 0
    text = capsys.readouterr().out
    assert "CONFLICT-PREVIEW tips=3 tips_with_conflicts=1 conflicted_paths=1" in text
    marked = (out / f"02-{t2}" / "a.txt").read_text()
    assert "<<<<<<<" in marked and "ONE" in marked and "TWO" in marked
    summary = (out / f"02-{t2}" / "summary.txt").read_text()
    assert f"sha: {t2}" in summary and "a.txt  hunks=1" in summary and f"01-{t1}" in summary
    assert "conflicted paths: 0" in (out / f"01-{t1}" / "summary.txt").read_text()
    assert "conflicted paths: 0" in (out / f"03-{t3}" / "summary.txt").read_text()
    assert not (out / f"01-{t1}" / "a.txt").exists()
    # read-only: refs and worktree unchanged
    assert git(repo, "rev-parse", "main") == head and git(repo, "status", "--porcelain") == ""


def test_third_tip_conflicts_only_with_second_tips_result(repo, tmp_path, capsys):
    t1 = commit(repo, "t1", "main", {"b.txt": BASE.replace("line 2", "ONE")}, "t1")
    t2 = commit(repo, "t2", "main", {"a.txt": BASE.replace("line 5", "TWO")}, "t2")
    # t3 is based on t2's edit of a.txt and changes the same line again, so against main it merges
    # cleanly with t1 but not with t1+t2.
    t3 = commit(repo, "t3", t2, {"a.txt": BASE.replace("line 5", "THREE")}, "t3")
    out = tmp_path / "out"
    assert run(repo, out, t1, t3, t2) == 0
    text = capsys.readouterr().out
    # t3 (contains t2) merges clean; t2 afterwards is already contained: no folder entry for it
    assert "tips_with_conflicts=0" in text
    out2 = tmp_path / "out2"
    t2b = commit(repo, "t2b", "main", {"a.txt": BASE.replace("line 5", "TWOB")}, "t2b")
    assert run(repo, out2, t1, t2, t2b) == 0
    assert "<<<<<<<" in (out2 / f"03-{t2b}" / "a.txt").read_text()
    assert "01-" not in (out2 / f"03-{t2b}" / "summary.txt").read_text().split("a.txt")[1].split("\n")[0]
    assert f"02-{t2}" in (out2 / f"03-{t2b}" / "summary.txt").read_text()


def test_same_file_both_tips_heuristic(repo, tmp_path):
    t1 = commit(repo, "t1", "main", {"a.txt": BASE.replace("line 2", "X")}, "t1")
    t2 = commit(repo, "t2", "main", {"a.txt": BASE.replace("line 9", "Y")}, "t2")  # same file, no conflict
    out = tmp_path / "out"
    assert run(repo, out, t1, t2) == 0
    summary = (out / f"02-{t2}" / "summary.txt").read_text()
    assert "same file, both tips (heuristic, not a conflict): 1" in summary and f"a.txt  <- also in 01-{t1}" in summary


def test_conflict_dir_needs_dry_run(repo, tmp_path):
    t1 = commit(repo, "t1", "main", {"c.txt": "x\n"}, "t1")
    git(repo, "checkout", "-q", "main")
    with pytest.raises(SystemExit):
        lm.main(["--root", str(repo), "--batch", t1, "--conflict-dir", str(tmp_path / "o")])
