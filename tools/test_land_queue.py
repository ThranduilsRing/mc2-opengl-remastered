"""land_queue: queue logic against a fake repo and a fake runner (no builds, no gate)."""
import os
import subprocess
import sys
import threading

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.append(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
from tools import land_queue as lq  # noqa: E402
import console_child  # noqa: E402


def _git(root, *a):
    return subprocess.run(["git", *a], cwd=root, capture_output=True, text=True, check=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def _step(cmd):
    return next(a for a in cmd if a in ("--lint", "--precheck", "--batch"))


def _sha(cmd):
    return cmd[cmd.index(_step(cmd)) + 1]


def _real_git_else(result):
    def runner(cmd, cwd):
        return lq.default_runner(cmd, cwd) if cmd[0] == "git" else result
    return runner


def _land_runner(rc_for, commit_on=None):
    """Fake lane_merge: rc_for(step, sha) -> (rc, out); the --batch step makes a real commit when commit_on(sha)."""
    seen = []

    def runner(cmd, cwd):
        if cmd[0] == "git":
            return lq.default_runner(cmd, cwd)
        st, sha = _step(cmd), _sha(cmd)
        seen.append((st, sha))
        if st == "--batch" and commit_on and commit_on(sha):
            _git(cwd, "commit", "-q", "--allow-empty", "-m", "merge " + sha)
        return rc_for(st, sha)
    runner.seen = seen
    return runner


@pytest.fixture
def repo(tmp_path):
    r = tmp_path / "repo"
    r.mkdir()
    _git(r, "init", "-q", "-b", lq.NIFTY)
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
    r = _land_runner(lambda st, sha: (1, "ratchet red") if (st, sha) == ("--precheck", "bbb222") else (0, ""))
    assert lq.land(q, root, r) == 1
    assert r.seen == [("--lint", "aaa111"), ("--precheck", "aaa111"), ("--batch", "aaa111"),
                      ("--lint", "bbb222"), ("--precheck", "bbb222"),          # rejected: no merge step
                      ("--lint", "ccc333"), ("--precheck", "ccc333"), ("--batch", "ccc333")]
    st = {e["branch"]: e for e in lq.load(q)["queue"]}
    assert st["claude/a"]["status"] == "landed" and st["claude/c"]["status"] == "landed"
    assert st["claude/b"]["status"] == "rejected"
    assert "precheck rc=1" in st["claude/b"]["reason"] and "ratchet red" in st["claude/b"]["reason"]
    r.seen.clear()
    lq.land(q, root, r)
    assert r.seen == []                     # a rejected tip stays put until resubmitted
    lq.submit(q, "claude/b", "bbb333")      # a fixed tip replaces the rejected entry
    assert [e["status"] for e in lq.load(q)["queue"] if e["branch"] == "claude/b"] == ["queued"]


def test_lint_red_stops_before_precheck(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    r = _land_runner(lambda st, sha: (1, "file_size red"))
    lq.land(q, root, r)
    assert [s for s, _ in r.seen] == ["--lint"]
    assert lq.load(q)["queue"][0]["reason"].startswith("lint rc=1")


def test_batch_verify_full_is_requested(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    cmds = []

    def runner(cmd, cwd):
        if cmd[0] != "git":
            cmds.append(cmd)
            return 0, ""
        return lq.default_runner(cmd, cwd)
    lq.land(q, root, runner)
    assert cmds[2][-4:] == ["--batch", "aaa111", "--verify", "full"] and "--no-link" in cmds[1]


@pytest.mark.parametrize("rc", [5, 6, 7])
def test_post_merge_red_resets_nifty_and_next_tip_lands(repo, rc):
    root, q = repo
    pre = _git(root, "rev-parse", "HEAD")
    lq.submit(q, "claude/a", "aaa111")
    lq.submit(q, "claude/b", "bbb222")
    r = _land_runner(lambda st, sha: (rc, "ratchet red") if (st, sha) == ("--batch", "aaa111") else (0, ""),
                     commit_on=lambda sha: True)
    assert lq.land(q, root, r) == 1
    st = {e["branch"]: e for e in lq.load(q)["queue"]}
    assert st["claude/a"]["status"] == "rejected" and st["claude/a"]["reset_from"]
    assert st["claude/b"]["status"] == "landed"
    assert _git(root, "rev-parse", "HEAD~1") == pre     # a's kept merge commit is gone, b's commit sits on pre


@pytest.mark.parametrize("rc,out", [(3, "MERGE_HEAD present"), (4, "staged files"), (5, "precheck tree busy: lock x"),
                                    (1, "REFUSED: journal lock"), (2, "PARTIAL: skipped rows")])
def test_infra_failure_requeues_and_stops_the_loop(repo, rc, out):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    lq.submit(q, "claude/b", "bbb222")
    r = _land_runner(lambda st, sha: (rc, out) if st == "--precheck" else (0, ""))
    assert lq.land(q, root, r) == lq.EX_BUSY
    st = {e["branch"]: e for e in lq.load(q)["queue"]}
    assert st["claude/a"]["status"] == "queued" and "infra" in st["claude/a"]["reason"]
    assert st["claude/b"]["status"] == "queued"
    assert all(sha == "aaa111" for _, sha in r.seen)       # b was never tried


def test_real_conflict_rc5_is_a_tip_red(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    lq.land(q, root, _land_runner(lambda st, sha: (5, "CONFLICT in x") if st == "--precheck" else (0, "")))
    assert lq.load(q)["queue"][0]["status"] == "rejected"


def test_dirty_nifty_tree_touches_no_entry(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    with open(os.path.join(root, "f"), "w") as f:
        f.write("dirty")
    r = _land_runner(lambda st, sha: (0, ""))
    assert lq.land(q, root, r) == lq.EX_BUSY and r.seen == []
    assert lq.load(q)["queue"][0]["status"] == "queued"


def test_wrong_branch_is_refused(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    _git(root, "checkout", "-q", "-b", "other")
    r = _land_runner(lambda st, sha: (0, ""))
    assert lq.land(q, root, r) == lq.EX_BUSY and r.seen == []


def test_second_land_and_gate_get_busy_and_submit_during_land_is_not_lost(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    inner, t = {}, []

    def rc_for(st, sha):
        if st == "--lint":
            inner["land"] = lq.land(q, root, _land_runner(lambda *a: (0, "")))
            inner["gate"] = lq.gate(q, root, False, _land_runner(lambda *a: (0, "")))
            inner["submit0"] = lq.submit(q, "claude/x", "xxx999", wait_s=0)
            t.append(threading.Thread(target=lambda: inner.__setitem__("submit", lq.submit(q, "claude/y", "yyy000", wait_s=20))))
            t[0].start()
        return 0, ""
    assert lq.land(q, root, _land_runner(rc_for)) == 0
    t[0].join(30)
    assert inner["land"] == inner["gate"] == inner["submit0"] == lq.EX_BUSY and inner["submit"] == 0
    names = {e["branch"]: e["status"] for e in lq.load(q)["queue"]}
    assert names == {"claude/a": "landed", "claude/y": "queued"}


def test_corrupt_queue_is_restored_from_bak_and_save_is_atomic(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    lq.submit(q, "claude/b", "bbb222")         # the second save leaves a .bak holding the state with a only
    assert os.path.exists(q + ".bak") and not os.path.exists(q + ".tmp")
    with open(q, "w") as f:
        f.write("{ not json")
    assert [e["branch"] for e in lq.load(q)["queue"]] == ["claude/a"]
    assert any(n.startswith("q.json.corrupt-") for n in os.listdir(os.path.dirname(q)))
    with open(q, "w") as f:
        f.write("{ not json")
    os.remove(q + ".bak")
    with pytest.raises(lq.Busy):
        lq.load(q)


def test_dry_run_changes_nothing(repo):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    r = _land_runner(lambda st, sha: (0, ""))
    assert lq.land(q, root, r, dry_run=True) == 0 and r.seen == []
    assert lq.load(q)["queue"][0]["status"] == "queued"


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
