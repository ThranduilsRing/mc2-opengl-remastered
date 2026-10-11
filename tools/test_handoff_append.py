"""Tests for tools/handoff_append.py (throwaway git repo mirroring the real handoff's structure; stdlib + git only).

Run:
  py -3 -B -m pytest tools/test_handoff_append.py -q -p no:cacheprovider
"""
import os
import re
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

TOOL = os.path.join(HERE, "handoff_append.py")
REL = ".claude/cleanup/ORCHESTRATOR-HANDOFF.md"
LOG_REL = ".claude/cleanup/HANDOFF-LOG.md"

DOC = (
    "<!-- doc-status: current | reviewed:2026-10-11 -->\n"
    "# Handoff\n"
    "\n"
    "## Ground rules\n"
    "- Rule one.\n"
    "\n"
    "## Lanes now\n"
    "- Lanes (2026-10-11 10:30): old lane text.\n"
    "- State files: x.\n"
    "\n"
    "## Ordered queue\n"
    "1. ~~S1~~ DONE. alpha beta\n"
    "2. **S2** gamma delta gamma\n"
    "3. **S3** epsilon\n"
    "\n"
    "Parked visual work: later.\n"
    "\n"
    "## Session exit bar\n"
    "Bar.\n"
)

LOG = (
    "<!-- doc-status: current | reviewed:2026-10-11 -->\n"
    "# Handoff log\n"
    "\n"
    "Intro.\n"
    "\n"
    "## Entries (newest first)\n"
    "\n"
    "- [2026-10-10] Newest old entry.\n"
    "- [2026-10-09] Oldest entry.\n"
    "\n"
    "## Archive: old queue\n"
    "Archived text.\n"
)

STAMP = re.compile(r"^- \[\d{4}-\d\d-\d\d \d\d:\d\d\] ")


def run_git(cwd, *args):
    r = subprocess.run(["git", "-C", str(cwd), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def tool(repo, *args):
    r = subprocess.run([sys.executable, "-B", TOOL, "--root", str(repo), *args], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True, **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout, r.stderr


@pytest.fixture
def repo(tmp_path):
    run_git(tmp_path, "init", "-q")
    run_git(tmp_path, "config", "user.email", "t@example.com")
    run_git(tmp_path, "config", "user.name", "t")
    doc = tmp_path / REL
    doc.parent.mkdir(parents=True)
    doc.write_bytes(DOC.encode("utf-8"))
    (tmp_path / LOG_REL).write_bytes(LOG.encode("utf-8"))
    run_git(tmp_path, "add", "-A")
    run_git(tmp_path, "commit", "-q", "-m", "init")
    return tmp_path


def text(repo):
    return (repo / REL).read_bytes().decode("utf-8")


def log_text(repo):
    return (repo / LOG_REL).read_bytes().decode("utf-8")


def committed_files(repo):
    return sorted(run_git(repo, "show", "--name-only", "--format=", "HEAD").split("\n"))


def test_lanes_replaces_bullet_and_commits(repo):
    rc, out, err = tool(repo, "--lanes", "new lane text", "--message", "handoff: lanes")
    assert rc == 0, err
    t = text(repo)
    assert "old lane text" not in t
    line = next(ln for ln in t.split("\n") if ln.startswith("- Lanes ("))
    assert line.endswith("): new lane text")
    assert len(line.split("(")[1].split(")")[0]) == len("2026-10-11 10:30")
    assert t.split("\n")[0] == DOC.split("\n")[0]
    assert out.strip() == run_git(repo, "rev-parse", "HEAD")
    assert run_git(repo, "log", "-1", "--format=%s") == "handoff: lanes"
    assert "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" in run_git(repo, "log", "-1", "--format=%B")
    assert committed_files(repo) == [REL]
    assert log_text(repo) == LOG


def test_bullet_goes_to_log_newest_first_with_stamp(repo):
    rc, _, err = tool(repo, "--bullet", "fresh milestone")
    assert rc == 0, err
    lines = log_text(repo).split("\n")
    i = lines.index("## Entries (newest first)")
    assert lines[i + 1] == ""
    assert STAMP.match(lines[i + 2]) and lines[i + 2].endswith("] fresh milestone")
    assert lines[i + 3:i + 5] == ["- [2026-10-10] Newest old entry.", "- [2026-10-09] Oldest entry."]
    assert text(repo) == DOC
    assert committed_files(repo) == [LOG_REL]


def test_several_bullets_last_given_ends_on_top(repo):
    assert tool(repo, "--bullet", "one", "--bullet", "two")[0] == 0
    entries = [ln for ln in log_text(repo).split("\n") if ln.startswith("- [")]
    assert [e.split("] ", 1)[1] for e in entries] == ["two", "one", "Newest old entry.", "Oldest entry."]


def test_bullet_leaves_archive_and_other_lines_alone(repo):
    assert tool(repo, "--bullet", "x")[0] == 0
    old, new = LOG.split("\n"), log_text(repo).split("\n")
    added = [ln for ln in new if ln not in old]
    assert len(added) == 1 and STAMP.match(added[0])
    assert [ln for ln in old if ln not in new] == []
    assert new[-3:] == ["## Archive: old queue", "Archived text.", ""]


def test_queue_prefix_replace_append(repo):
    rc, _, err = tool(repo, "--queue-prefix", "3", "**DONE 2026-10-11**", "--queue-replace", "1", "alpha", "ALPHA",
                      "--queue-append", "fresh item")
    assert rc == 0, err
    lines = text(repo).split("\n")
    assert "3. **DONE 2026-10-11** **S3** epsilon" in lines
    assert "1. ~~S1~~ DONE. ALPHA beta" in lines
    i = lines.index("3. **DONE 2026-10-11** **S3** epsilon")
    assert lines[i + 1:i + 3] == ["4. fresh item", ""]
    assert "Parked visual work: later." in lines
    assert committed_files(repo) == [REL]


def test_other_sections_untouched(repo):
    assert tool(repo, "--queue-append", "y")[0] == 0
    old, new = DOC.split("\n"), text(repo).split("\n")
    assert [ln for ln in new if ln not in old] == ["4. y"]
    assert [ln for ln in old if ln not in new] == []


def test_live_and_log_edits_share_one_commit(repo):
    head = run_git(repo, "rev-parse", "HEAD")
    rc, out, err = tool(repo, "--bullet", "milestone", "--lanes", "L", "--message", "handoff: both")
    assert rc == 0, err
    assert run_git(repo, "rev-list", "--count", f"{head}..HEAD") == "1"
    assert committed_files(repo) == sorted([REL, LOG_REL])
    assert out.strip() == run_git(repo, "rev-parse", "HEAD")


def test_queue_prefix_is_idempotent(repo):
    assert tool(repo, "--queue-prefix", "2", "DONE.")[0] == 0
    before = text(repo)
    head = run_git(repo, "rev-parse", "HEAD")
    rc, _, err = tool(repo, "--queue-prefix", "2", "DONE.")
    assert rc == 2 and "already" in err
    assert text(repo) == before and run_git(repo, "rev-parse", "HEAD") == head


def test_unknown_item_exit_2(repo):
    for args in (["--queue-prefix", "9", "x"], ["--queue-replace", "9", "a", "b"]):
        rc, _, err = tool(repo, *args)
        assert rc == 2 and "not found" in err
    assert text(repo) == DOC


def test_replace_absent_or_ambiguous_exit_2(repo):
    assert tool(repo, "--queue-replace", "1", "nothere", "x")[0] == 2
    assert tool(repo, "--queue-replace", "2", "gamma", "x")[0] == 2  # occurs twice
    assert tool(repo, "--queue-replace", "2", "delta", "x")[0] == 0


def test_replace_cannot_reach_other_items(repo):
    assert tool(repo, "--queue-replace", "1", "epsilon", "x")[0] == 2


def test_no_operation_exit_2(repo):
    assert tool(repo)[0] == 2


def test_missing_sections_exit_2(repo):
    (repo / LOG_REL).write_bytes(b"# no entries heading\n")
    run_git(repo, "commit", "-q", "-am", "strip log")
    rc, _, err = tool(repo, "--bullet", "x")
    assert rc == 2 and "Entries" in err
    rc, _, err = tool(repo, "--log", "nope.md", "--bullet", "x")
    assert rc == 2 and "not found" in err


def test_dry_run_shows_both_files_and_changes_nothing(repo):
    head = run_git(repo, "rev-parse", "HEAD")
    rc, out, _ = tool(repo, "--dry-run", "--bullet", "preview", "--queue-append", "item")
    assert rc == 0 and "dry-run" in out
    assert f"--- {LOG_REL}" in out and f"--- {REL}" in out
    assert "] preview" in out and "+4. item" in out
    assert text(repo) == DOC and log_text(repo) == LOG and run_git(repo, "rev-parse", "HEAD") == head
    assert run_git(repo, "status", "--porcelain") == ""


def test_refuses_with_merge_head(repo):
    (repo / ".git" / "MERGE_HEAD").write_text(run_git(repo, "rev-parse", "HEAD") + "\n")
    rc, _, err = tool(repo, "--bullet", "x")
    assert rc == 3 and "merge in progress" in err
    assert log_text(repo) == LOG


def test_refuses_with_unmerged_paths(repo):
    base = run_git(repo, "rev-parse", "--abbrev-ref", "HEAD")
    (repo / "f.txt").write_text("base\n")
    run_git(repo, "add", "f.txt")
    run_git(repo, "commit", "-q", "-m", "f")
    run_git(repo, "checkout", "-q", "-b", "other")
    (repo / "f.txt").write_text("other\n")
    run_git(repo, "commit", "-q", "-am", "o")
    run_git(repo, "checkout", "-q", base)
    (repo / "f.txt").write_text("mine\n")
    run_git(repo, "commit", "-q", "-am", "m")
    r = subprocess.run(["git", "-C", str(repo), "merge", "other"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode != 0
    (repo / ".git" / "MERGE_HEAD").unlink()  # isolate the UU-path refusal from the MERGE_HEAD one
    rc, _, err = tool(repo, "--bullet", "x")
    assert rc == 3 and "unmerged" in err


def test_refuses_when_a_target_doc_has_foreign_edits(repo):
    (repo / LOG_REL).write_bytes((LOG + "foreign edit\n").encode("utf-8"))
    rc, _, err = tool(repo, "--bullet", "x")
    assert rc == 3 and "uncommitted" in err
    # the live doc is not a target of a bullet-only run, and the dirty log is not a target of a live-only run
    assert tool(repo, "--lanes", "L")[0] == 0
    assert committed_files(repo) == [REL]
    (repo / REL).write_bytes((text(repo) + "foreign edit\n").encode("utf-8"))
    rc, _, err = tool(repo, "--lanes", "M")
    assert rc == 3 and "uncommitted" in err


def test_commit_does_not_sweep_other_staged_files(repo):
    (repo / "other.txt").write_text("staged by another lane\n")
    run_git(repo, "add", "other.txt")
    assert tool(repo, "--bullet", "x", "--lanes", "y")[0] == 0
    assert committed_files(repo) == sorted([REL, LOG_REL])
    assert run_git(repo, "diff", "--cached", "--name-only") == "other.txt"


def test_lf_endings_preserved(repo):
    assert tool(repo, "--bullet", "x", "--lanes", "y")[0] == 0
    assert b"\r" not in (repo / REL).read_bytes()
    assert b"\r" not in (repo / LOG_REL).read_bytes()
    assert log_text(repo).split("\n")[0] == LOG.split("\n")[0]
