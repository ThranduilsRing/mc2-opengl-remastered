"""Tests for tools/lane_merge.py (throwaway git repo + fake ratchet scripts; stdlib + git only).

Run:
  py -3 -B -m pytest tools/test_lane_merge.py -q -p no:cacheprovider
"""
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

TOOL = os.path.join(HERE, "lane_merge.py")

FSIZE = "tools/file_size_ratchet.baseline.json"
CXX = "tools/cxx17_ratchet/baseline.json"
DUP = "tools/dup_lines_ratchet.baseline.json"
AXIS = "tools/axis_swizzle_ratchet.baseline.json"
SHADERS = "scripts/gl_shader_variants.json"

# Fake ratchets: behaviour is picked by a <script>.mode file next to the script.
#   ok (default) -> exit 0; fail -> exit 1 with output; tighten -> rewrite BASELINE (+ a decoy) and exit 0
FAKE = '''import os, sys
here = os.path.dirname(os.path.abspath(__file__))
name = os.path.splitext(os.path.basename(__file__))[0]
mode_file = os.path.join(here, name + ".mode")
mode = open(mode_file).read().strip() if os.path.exists(mode_file) else "ok"
BASELINE = %r
if mode == "fail":
    for i in range(6):
        print("line %%d of %%s" %% (i, name))
    sys.exit(1)
if mode == "tighten":
    if BASELINE:
        open(BASELINE, "w").write("tightened by " + name + "\\n")
    open("tools/decoy.json", "w").write("decoy " + name + "\\n")
sys.exit(0)
'''
FAKES = {
    "file_size_ratchet.py": FSIZE,
    "cxx17_ratchet/cxx17_ratchet.py": CXX,
    "dup_lines_ratchet.py": DUP,
    "axis_swizzle_ratchet.py": AXIS,
    "split/split_map.py": None,
}


def git(cwd, *args, check=True):
    r = subprocess.run(["git", "-C", str(cwd), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       **console_child.child_kwargs(redirected=True))
    if check:
        assert r.returncode == 0, r.stdout
    return r.stdout.strip()


def write(repo, rel, content):
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content, encoding="utf-8", newline="\n")


def commit_all(repo, msg):
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", msg)


@pytest.fixture
def env(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    for rel in (FSIZE, CXX, DUP, AXIS, SHADERS):
        write(repo, rel, "base\n")
    write(repo, "a.txt", "a\n")
    write(repo, "b.txt", "b\n")
    commit_all(repo, "base")
    tools = tmp_path / "faketools"
    for rel, baseline in FAKES.items():
        write(tools, rel, FAKE % baseline)
    return repo, tools


def lane_branch(repo, edits, subject="lane work"):
    """Create branch `lane` off HEAD with the given {path: content}, return to main."""
    git(repo, "checkout", "-q", "-b", "lane")
    for rel, content in edits.items():
        write(repo, rel, content)
    commit_all(repo, subject)
    git(repo, "checkout", "-q", "main")


def merge(env_, *extra, ref="lane"):
    repo, tools = env_
    r = subprocess.run([sys.executable, "-B", TOOL, ref, "--root", str(repo), "--tools-dir", str(tools), *extra],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout, r.stderr


def set_mode(env_, rel, mode):
    (env_[1] / (os.path.splitext(rel)[0] + ".mode")).write_text(mode, encoding="utf-8")


def parents(repo, rev="HEAD"):
    return git(repo, "rev-list", "--parents", "-n", "1", rev).split()[1:]


def test_clean_merge(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"}, "lane: add thing")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    rc, out, err = merge(env)
    assert rc == 0, err
    assert out.startswith("MERGED ") and "files=1 ratchets=OK overlap=0" in out and "(+" not in out
    assert len(parents(repo)) == 2
    assert git(repo, "log", "-1", "--format=%s") == "merge lane: lane: add thing"
    assert "Co-Authored-By: Claude Fable 5.1" in git(repo, "log", "-1", "--format=%B")
    assert (repo / "lane.txt").exists() and (repo / "main.txt").exists()


def test_message_override(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    assert merge(env, "--message", "custom subject")[0] == 0
    assert git(repo, "log", "-1", "--format=%s") == "custom subject"


def test_dirty_overlap_refused(env):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n", "lane.txt": "x\n"})
    write(repo, "a.txt", "dirty a\n")
    head = git(repo, "rev-parse", "HEAD")
    rc, out, err = merge(env)
    assert rc == 4 and "a.txt" in err and "lane.txt" not in err
    assert git(repo, "rev-parse", "HEAD") == head
    assert not (repo / ".git" / "MERGE_HEAD").exists()
    assert (repo / "a.txt").read_text() == "dirty a\n"


def test_dirty_untracked_overlap_refused(env):
    repo, _ = env
    lane_branch(repo, {"new.txt": "lane\n"})
    write(repo, "new.txt", "untracked collision\n")
    rc, _, err = merge(env)
    assert rc == 4 and "new.txt" in err


def test_unrelated_dirty_file_does_not_block(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "b.txt", "dirty b\n")
    write(repo, "scratch.txt", "untracked\n")
    rc, out, err = merge(env)
    assert rc == 0, err
    assert (repo / "b.txt").read_text() == "dirty b\n"
    assert git(repo, "status", "--porcelain").count("b.txt") == 1


def test_staged_changes_refused(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "b.txt", "staged b\n")
    git(repo, "add", "b.txt")
    rc, _, err = merge(env)
    assert rc == 4 and "staged" in err and "b.txt" in err


def test_regenerable_conflict_resolved_to_lane(env):
    repo, _ = env
    lane_branch(repo, {FSIZE: "lane baseline\n", SHADERS: "lane variants\n"})
    write(repo, FSIZE, "main baseline\n")
    write(repo, SHADERS, "main variants\n")
    commit_all(repo, "main edits baselines")
    rc, out, err = merge(env)
    assert rc == 0, err
    assert (repo / FSIZE).read_text() == "lane baseline\n"
    assert (repo / SHADERS).read_text() == "lane variants\n"
    assert len(parents(repo)) == 2
    assert git(repo, "status", "--porcelain") == ""


def test_non_regenerable_conflict_aborts(env):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n", FSIZE: "lane baseline\n"})
    write(repo, "a.txt", "main a\n")
    write(repo, FSIZE, "main baseline\n")
    commit_all(repo, "main edits")
    head = git(repo, "rev-parse", "HEAD")
    rc, out, err = merge(env)
    assert rc == 5 and "a.txt" in err and FSIZE not in err
    assert git(repo, "rev-parse", "HEAD") == head
    assert not (repo / ".git" / "MERGE_HEAD").exists()
    assert git(repo, "status", "--porcelain") == ""
    assert (repo / "a.txt").read_text() == "main a\n"


def test_ratchet_failure_exit_6_commit_kept(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "cxx17_ratchet/cxx17_ratchet.py", "fail")
    rc, out, err = merge(env)
    assert rc == 6
    assert "ratchets=FAIL" in out
    assert "line 5 of cxx17_ratchet" in err and "line 3 of cxx17_ratchet" in err and "line 2 of" not in err
    assert len(parents(repo)) == 2  # the merge commit stays


def test_followup_commits_only_the_named_baselines(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "file_size_ratchet.py", "tighten")
    set_mode(env, "cxx17_ratchet/cxx17_ratchet.py", "tighten")
    rc, out, err = merge(env)
    assert rc == 0, err
    assert "(+" in out and "ratchets=OK" in out
    files = git(repo, "show", "--name-only", "--format=", "HEAD").split()
    assert sorted(files) == sorted([FSIZE, CXX])
    assert git(repo, "log", "-1", "--format=%s") == "merge follow-up: ratchet baselines tightened at merged HEAD"
    assert "Co-Authored-By: Claude Fable 5.1" in git(repo, "log", "-1", "--format=%B")
    assert len(parents(repo, "HEAD")) == 1 and len(parents(repo, "HEAD~1")) == 2
    assert "tools/decoy.json" in git(repo, "status", "--porcelain")  # decoy never committed


def test_followup_skips_baseline_that_was_already_dirty(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    write(repo, AXIS, "someone else's edit\n")
    set_mode(env, "file_size_ratchet.py", "tighten")
    set_mode(env, "axis_swizzle_ratchet.py", "tighten")
    rc, out, err = merge(env)
    assert rc == 0, err
    assert git(repo, "show", "--name-only", "--format=", "HEAD").split() == [FSIZE]
    assert "left alone" in err and AXIS in err


def test_merge_in_progress_refused(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    (repo / ".git" / "MERGE_HEAD").write_text(git(repo, "rev-parse", "HEAD") + "\n")
    rc, _, err = merge(env)
    assert rc == 3 and "in progress" in err


def test_bad_ref_exit_2(env):
    rc, _, err = merge(env, ref="no-such-branch")
    assert rc == 2


def test_already_contained_is_noop(env):
    repo, _ = env
    head = git(repo, "rev-parse", "HEAD")
    rc, out, _ = merge(env, ref="HEAD")
    assert rc == 0 and "already contained" in out
    assert git(repo, "rev-parse", "HEAD") == head


def test_dry_run_changes_nothing_and_predicts_conflicts(env):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n", FSIZE: "lane baseline\n"})
    write(repo, "a.txt", "main a\n")
    write(repo, FSIZE, "main baseline\n")
    commit_all(repo, "main edits")
    head = git(repo, "rev-parse", "HEAD")
    rc, out, _ = merge(env, "--dry-run")
    assert rc == 0
    assert "PLAN conflicts=2 regenerable=1 blocking=1" in out and "a.txt" in out and "dry-run" in out
    assert git(repo, "rev-parse", "HEAD") == head
    assert git(repo, "status", "--porcelain") == ""
    assert not (repo / ".git" / "MERGE_HEAD").exists()


def test_dry_run_reports_overlap_refusal(env):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n"})
    write(repo, "a.txt", "dirty\n")
    rc, out, err = merge(env, "--dry-run")
    assert rc == 4 and "overlap=1" in out and "a.txt" in err


# ---- a merge that dies part-way (a file in the worktree is locked) --------------------------------------------------------------------

sys.path.insert(0, HERE)
import lane_merge as lm  # noqa: E402

LOCKED = "error: unable to unlink old 'a.txt': Invalid argument\nfatal: merge failed"


class _FlakyMerge:
    """Wraps lane_merge.git: the first `merge --no-ff` writes the lane's new file (untracked), then fails like a locked-file merge."""

    def __init__(self, repo, real, message=LOCKED, fail_times=1):
        self.repo, self.real, self.message, self.left = repo, real, message, fail_times

    def __call__(self, root, *args):
        if args[:2] == ("merge", "--no-ff") and self.left > 0:
            self.left -= 1
            write(self.repo, "lane_new.txt", "new\n")
            return subprocess.CompletedProcess(args, 1, stdout=self.message)
        return self.real(root, *args)


def _call_do_merge(repo, monkeypatch, flaky, **kw):
    monkeypatch.setattr(lm, "git", flaky)
    lm.do_merge(str(repo), git(repo, "rev-parse", "lane"), "merge lane", lane_files=["lane_new.txt"], **kw)


def test_failed_merge_removes_only_the_lanes_untracked_files(env, monkeypatch):
    repo, _ = env
    lane_branch(repo, {"lane_new.txt": "new\n"})
    write(repo, "mine_untracked.txt", "keep\n")
    flaky = _FlakyMerge(repo, lm.git, message="fatal: some other failure")
    with pytest.raises(lm.Stop) as e:
        _call_do_merge(repo, monkeypatch, flaky)
    assert e.value.code == 5 and "lane_new.txt" in str(e.value)
    assert not (repo / "lane_new.txt").exists() and (repo / "mine_untracked.txt").exists()


def test_locked_file_waits_for_the_tidy_lock_and_retries_once(env, monkeypatch):
    repo, _ = env
    lane_branch(repo, {"lane_new.txt": "new\n"})
    lock = repo / "build-alltargets" / ".tidy.lock"
    write(repo, "build-alltargets/.tidy.lock", "{}")
    waits = []

    def fake_sleep(s):
        waits.append(s)
        lock.unlink()

    _call_do_merge(repo, monkeypatch, _FlakyMerge(repo, lm.git), sleep=fake_sleep)
    assert waits == [10] and len(parents(repo)) == 2 and (repo / "lane_new.txt").read_text() == "new\n"


def test_locked_file_without_a_tidy_lock_fails_at_once(env, monkeypatch):
    repo, _ = env
    lane_branch(repo, {"lane_new.txt": "new\n"})
    with pytest.raises(lm.Stop) as e:
        _call_do_merge(repo, monkeypatch, _FlakyMerge(repo, lm.git), sleep=lambda s: pytest.fail("must not wait"))
    assert e.value.code == 5 and not (repo / "lane_new.txt").exists()


def test_lock_that_never_clears_gives_up_and_cleans(env, monkeypatch):
    repo, _ = env
    lane_branch(repo, {"lane_new.txt": "new\n"})
    write(repo, "build-alltargets/.tidy.lock", "{}")
    ticks = iter(range(0, 10000, 100))
    with pytest.raises(lm.Stop) as e:
        _call_do_merge(repo, monkeypatch, _FlakyMerge(repo, lm.git), sleep=lambda s: None, clock=lambda: next(ticks),
                       lock_wait_s=300)
    assert e.value.code == 5 and not (repo / "lane_new.txt").exists() and (repo / "build-alltargets" / ".tidy.lock").exists()
