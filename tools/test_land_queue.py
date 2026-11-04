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


@pytest.mark.parametrize("rc,out", [(3, "MERGE_HEAD present"), (4, "staged files"), (5, "lane_merge: precheck tree busy: lock x"),
                                    (1, "[merge_gate] REFUSED: journal lock"),
                                    (2, "[merge_gate] clock: 1.0 min verdict=PARTIAL REUSED=none SKIPPED=a")])
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




# ---- LQ-2: window counter, gate classification, tags, publish ----

def _gate_runner(gate_rc=0, gate_out="", pre_rc=0, pre_out=""):
    seen = []

    def runner(cmd, cwd):
        if cmd[0] == "git":
            return lq.default_runner(cmd, cwd)
        if "--preflight" in cmd:
            seen.append(("preflight", cmd[cmd.index("--window") + 1]))
            return pre_rc, pre_out
        if "--publish" in cmd:
            seen.append(("publish", cmd[cmd.index("--expect-head") + 1]))
            return 0, ""
        seen.append(("gate", cmd[cmd.index("--window") + 1], "--resume" in cmd))
        return gate_rc, gate_out
    runner.seen = seen
    return runner


@pytest.fixture
def proofs(tmp_path):
    p = tmp_path / "proofs"
    (p / "merge114").mkdir(parents=True)
    (p / "merge114-gate").mkdir()
    return str(p)


def _commit(root, msg):
    _git(root, "commit", "-q", "--allow-empty", "-m", msg)


def test_window_is_max_merge_dir_plus_one_and_green_is_tagged(repo, proofs):
    root, q = repo
    r = _gate_runner()
    assert lq.gate(q, root, runner=r, proofs=proofs) == 0
    assert r.seen == [("preflight", "115"), ("gate", "115", False)]
    assert _git(root, "cat-file", "-t", "nifty-green-115") == "tag"
    assert lq.load(q)["suspect"] == []
    assert lq.gate(q, root, runner=r, proofs=proofs) == 0   # already tagged: nothing to gate
    assert len(r.seen) == 2


def test_preflight_failure_is_infra_and_consumes_no_window(repo, proofs):
    root, q = repo
    r = _gate_runner(pre_rc=1, pre_out="preflight FAIL")
    assert lq.gate(q, root, runner=r, proofs=proofs) == lq.EX_BUSY
    s = lq.load(q)
    assert not s.get("window") and s["suspect"] == [] and not s.get("gate_infra")
    assert lq.gate(q, root, runner=_gate_runner(), proofs=proofs) == 0
    assert _git(root, "tag", "-l", "nifty-green-*") == "nifty-green-115"


def test_runtime_red_records_stages_range_and_next_head_gets_next_window(repo, proofs):
    root, q = repo
    assert lq.gate(q, root, runner=_gate_runner(), proofs=proofs) == 0
    _commit(root, "m1")
    _commit(root, "m2")
    red = _gate_runner(1, "x\n[merge_gate] failed stages: smoke (st), ratchets (st)\n")
    assert lq.gate(q, root, runner=red, proofs=proofs) == 1
    sus = lq.load(q)["suspect"]
    assert len(sus) == 1 and sus[0]["red_stages"] == ["smoke", "ratchets"] and sus[0]["window"] == 116
    assert [c.split(" ", 1)[1] for c in sus[0]["commits"]] == ["m2", "m1"]
    _commit(root, "m3")
    r = _gate_runner()
    assert lq.gate(q, root, runner=r, proofs=proofs) == 0
    assert r.seen[-1] == ("gate", "117", False)
    assert lq.load(q)["suspect"] == []


@pytest.mark.parametrize("rc,out,kind", [(1, "[merge_gate] REFUSED: dirty", "infra"), (2, "usage: merge_gate", "infra"),
                                         (2, "PARTIAL: 3 stages", "partial"), (1, "no stage line", "infra")])
def test_non_runtime_reds_never_mark_a_suspect_range(repo, proofs, rc, out, kind):
    root, q = repo
    assert lq.gate(q, root, runner=_gate_runner(rc, out), proofs=proofs) == lq.EX_BUSY
    s = lq.load(q)
    assert s["suspect"] == [] and s["gate_infra"][0]["kind"] == kind


def test_resume_at_same_head_keeps_its_window(repo, proofs):
    root, q = repo
    lq.gate(q, root, runner=_gate_runner(2, "PARTIAL"), proofs=proofs)
    r = _gate_runner()
    assert lq.gate(q, root, runner=r, proofs=proofs, resume=True) == 0
    assert r.seen[-1] == ("gate", "115", True)


def test_publish_only_for_tag_commit(repo, proofs):
    root, q = repo
    r = _gate_runner()
    assert lq.gate(q, root, publish=True, runner=r, proofs=proofs) == 0
    head = _git(root, "rev-parse", "HEAD")
    assert r.seen[-1] == ("publish", head)
    _commit(root, "later")
    assert lq.publish_tag(q, root, 115, runner=r) == 1
    assert r.seen[-1] == ("publish", head)   # no second publish


def test_known_failure_renewal_works_with_new_counter(tmp_path):
    import json
    import mod_smoke
    src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "mod_known_failures.json")
    rows = json.load(open(src))
    rows = rows["failures"] if isinstance(rows, dict) and "failures" in rows else rows
    for r in rows:
        r["first_window"], r["expires_window"] = 115, 118
    out = tmp_path / "kf.json"
    out.write_text(json.dumps(rows))
    assert mod_smoke.load_known_failures(str(out))


@pytest.mark.parametrize("rc,out", [(5, "lane_merge: BATCH REFUSED: a tip would abort on the blocking conflict(s) above"),
                                    (1, "Vulkan device profile REFUSED (no matching GPU)"), (1, "lint PARTIAL coverage note")])
def test_real_reds_with_refused_or_partial_text_are_rejected_not_infra(repo, rc, out):
    root, q = repo
    lq.submit(q, "claude/a", "aaa111")
    r = _land_runner(lambda st, sha: (rc, out) if st == "--precheck" else (0, ""))
    assert lq.land(q, root, r) == 1
    assert {e["branch"]: e for e in lq.load(q)["queue"]}["claude/a"]["status"] == "rejected"


def test_gate_runtime_red_mentioning_refused_stays_runtime(repo, proofs):
    root, q = repo
    out = "Vulkan device profile REFUSED" + chr(10) + "[merge_gate] failed stages: smoke (1)"
    assert lq.classify_gate(1, out) == ("runtime", ["smoke"])
    assert lq.classify_gate(1, "[merge_gate] REFUSED: dirty")[0] == "infra"
