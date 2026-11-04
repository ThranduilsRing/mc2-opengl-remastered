"""land_queue: queue logic against a fake repo and a fake runner (no builds, no gate)."""
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.append(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
from tools import land_queue as lq  # noqa: E402
import console_child  # noqa: E402


def _git(root, *a):
    return subprocess.run(["git", *a], cwd=root, capture_output=True, text=True, check=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def _real_git_else(result):
    def runner(cmd, cwd):
        return lq.default_runner(cmd, cwd) if cmd[0] == "git" else result
    return runner


@pytest.fixture
def repo(tmp_path):
    r = tmp_path / "repo"
    r.mkdir()
    _git(r, "init", "-q")
    _git(r, "config", "user.email", "t@t")
    _git(r, "config", "user.name", "t")
    for i in range(2):
        (r / "f").write_text(str(i))
        _git(r, "add", "f")
        _git(r, "commit", "-q", "-m", f"c{i}")
    return str(r), str(tmp_path / "q.json")


def test_land_runs_lint_precheck_merge_in_order_and_rejects_red(repo):
    root, q = repo
    for b, s in (("claude/a", "aaa111"), ("claude/b", "bbb222"), ("claude/c", "ccc333")):
        lq.submit(q, b, s)
    calls = []

    def runner(cmd, cwd):
        if cmd[0] == "git":
            return lq.default_runner(cmd, cwd)
        calls.append((cmd[-2] if cmd[-2].startswith("--") else "merge", cmd[-1]))
        red = cmd[-1] == "bbb222" and "--precheck" in cmd
        return (1, "ratchet red") if red else (0, "")
    lq.land(q, root, runner)
    assert calls == [("--lint", "aaa111"), ("--precheck", "aaa111"), ("merge", "aaa111"),
                     ("--lint", "bbb222"), ("--precheck", "bbb222"),          # rejected: no merge step
                     ("--lint", "ccc333"), ("--precheck", "ccc333"), ("merge", "ccc333")]
    st = {e["branch"]: e for e in lq.load(q)["queue"]}
    assert st["claude/a"]["status"] == "landed" and st["claude/c"]["status"] == "landed"
    assert st["claude/b"]["status"] == "rejected"
    assert "precheck rc=1" in st["claude/b"]["reason"] and "ratchet red" in st["claude/b"]["reason"]
    calls.clear()
    lq.land(q, root, runner)
    assert calls == []                      # a rejected tip stays put until resubmitted
    lq.submit(q, "claude/b", "bbb333")      # a fixed tip replaces the rejected entry
    assert [e["status"] for e in lq.load(q)["queue"] if e["branch"] == "claude/b"] == ["queued"]


def test_lint_red_stops_before_precheck(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    calls = []

    def runner(cmd, cwd):
        if cmd[0] == "git":
            return lq.default_runner(cmd, cwd)
        calls.append(cmd[-2])
        return 1, "file_size red"
    lq.land(q, root, runner)
    assert calls == ["--lint"]
    assert lq.load(q)["queue"][0]["reason"].startswith("lint rc=1")


def test_gate_green_tags_and_publishes_only_on_request(repo):
    root, q = repo
    seen = []

    def runner(cmd, cwd):
        if cmd[0] == "git":
            return lq.default_runner(cmd, cwd)
        seen.append("--publish" in cmd)
        return 0, ""
    assert lq.gate(q, root, False, runner) == 0
    assert lq.green_tags(root) == [1] and seen == [False]
    _git(root, "commit", "-q", "--allow-empty", "-m", "next")
    assert lq.gate(q, root, True, runner) == 0
    assert lq.green_tags(root) == [1, 2] and seen[-2:] == [False, True]   # gate run, then publish run


def test_gate_red_marks_range_since_last_green_suspect(repo):
    root, q = repo
    lq.gate(q, root, False, _real_git_else((0, "")))
    _git(root, "commit", "-q", "--allow-empty", "-m", "bad tip")
    assert lq.gate(q, root, True, _real_git_else((1, "stage red"))) == 1
    assert lq.green_tags(root) == [1]                       # no new tag, no publish
    s, = lq.load(q)["suspect"]
    assert s["from"] == "nifty-green-1" and "bad tip" in s["commits"][0]
    _git(root, "commit", "-q", "--allow-empty", "-m", "fix")
    lq.gate(q, root, False, _real_git_else((0, "")))        # the next green clears it
    assert lq.load(q)["suspect"] == [] and lq.green_tags(root) == [1, 2]
