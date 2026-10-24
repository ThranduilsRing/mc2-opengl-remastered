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
    rc, out, err = merge(env, "--trailer", "Co-Authored-By: Test Model <t@example.com>")
    assert rc == 0, err
    assert out.startswith("MERGED ") and "files=1 ratchets=OK verify=SKIPPED overlap=0" in out and "(+" not in out
    assert len(parents(repo)) == 2
    assert git(repo, "log", "-1", "--format=%s") == "merge lane: lane: add thing"
    assert "Co-Authored-By: Test Model" in git(repo, "log", "-1", "--format=%B")
    assert (repo / "lane.txt").exists() and (repo / "main.txt").exists()


def test_no_trailer_by_default(env):
    repo, _ = env
    lane_branch(repo, {"lane.txt": "x\n"})
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    assert merge(env)[0] == 0
    assert "Co-Authored-By" not in git(repo, "log", "-1", "--format=%B")


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
    rc, out, err = merge(env, "--trailer", "Co-Authored-By: Test Model <t@example.com>")
    assert rc == 0, err
    assert "(+" in out and "ratchets=OK" in out
    files = git(repo, "show", "--name-only", "--format=", "HEAD").split()
    assert sorted(files) == sorted([FSIZE, CXX])
    assert git(repo, "log", "-1", "--format=%s") == "merge follow-up: ratchet baselines tightened at merged HEAD"
    assert "Co-Authored-By: Test Model" in git(repo, "log", "-1", "--format=%B")
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
    assert rc == 5  # a predicted blocking conflict is the same rc as the real merge
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


def _load_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("lane_merge_mod", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _R:
    def __init__(self, rc, out=""):
        self.returncode, self.stdout = rc, out


def test_verify_runs_build_then_doctest_and_stops_at_the_first_failure(tmp_path):
    lm = _load_module()
    (tmp_path / "scripts").mkdir()
    for n in ("ninja_build.py", "run-unit-tests.sh"):
        (tmp_path / "scripts" / n).write_text("")
    calls = []

    def runner(root, cmd, env=None):
        assert env["MC2_NINJA_TREE"] == "build64-ninja"
        calls.append(os.path.basename(cmd[1]))
        return _R(1 if cmd[1].endswith("ninja_build.py") else 0, "boom")

    verdict, res = lm.verify_merged(str(tmp_path), "build", runner)
    assert verdict == "FAIL" and calls == ["ninja_build.py"] and res[0][0] == "build" and "boom" in res[0][2]
    calls.clear()
    verdict, res = lm.verify_merged(str(tmp_path), "build", lambda root, cmd, env=None: (calls.append(os.path.basename(cmd[1])), _R(0))[1])
    assert verdict == "OK" and calls == ["ninja_build.py", "run-unit-tests.sh"]


def test_git_bash_never_picks_the_wsl_launcher():
    lm = _load_module()
    gitexe = "C:\\Program Files\\Git\\cmd\\git.exe"
    gitbash = "C:\\Program Files\\Git\\usr\\bin\\bash.exe"
    which = lambda name: {"git": gitexe, "bash": "C:\\Windows\\System32\\bash.exe"}.get(name)
    assert lm.git_bash(which=which, exists=lambda p: p == gitbash, environ={}) == gitbash
    # no Git bash on disk: the PATH hit is refused when it is the WSL launcher or the Store alias
    assert lm.git_bash(which=which, exists=lambda p: False, environ={}) == "bash"
    assert lm.git_bash(which=lambda n: {"bash": "C:\\Users\\x\\AppData\\Local\\Microsoft\\WindowsApps\\bash.exe"}.get(n),
                       exists=lambda p: False, environ={}) == "bash"
    assert lm.git_bash(which=lambda n: {"bash": "/usr/bin/bash"}.get(n), exists=lambda p: False, environ={}) == "/usr/bin/bash"
    assert lm.git_bash(which=which, exists=lambda p: True, environ={"MC2_BASH": "X:/b.exe"}) == "X:/b.exe"


def test_verify_is_skipped_without_the_scripts_or_when_switched_off(tmp_path):
    lm = _load_module()
    assert lm.verify_merged(str(tmp_path), "build")[0] == "SKIPPED"
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "ninja_build.py").write_text("")
    assert lm.verify_merged(str(tmp_path), "none")[0] == "SKIPPED"


BUGS = ".claude/cleanup/BUGS-FOUND-FIXED.md"
HEAD_TXT = "# bugs\n\n| # | Bug |\n|---|---|\n"


def test_bugs_table_keeps_both_and_renumbers(env):
    repo, _ = env
    write(repo, BUGS, HEAD_TXT + "| 1 | one |\n\nfooter\n")
    commit_all(repo, "bugs base")
    lane_branch(repo, {BUGS: HEAD_TXT + "| 1 | one |\n| 2 | lane bug |\n\nfooter\n"})
    write(repo, BUGS, HEAD_TXT + "| 1 | one |\n| 2 | main bug |\n\nfooter\n")
    commit_all(repo, "main bug")
    rc, out, err = merge(env)
    assert rc == 0, err
    assert (repo / BUGS).read_text() == HEAD_TXT + "| 1 | one |\n| 2 | main bug |\n| 3 | lane bug |\n\nfooter\n"


def test_bugs_table_same_row_changed_by_both_returns_5(env):
    repo, _ = env
    write(repo, BUGS, HEAD_TXT + "| 1 | one |\n")
    commit_all(repo, "bugs base")
    lane_branch(repo, {BUGS: HEAD_TXT + "| 1 | lane edit |\n"})
    write(repo, BUGS, HEAD_TXT + "| 1 | main edit |\n")
    commit_all(repo, "main edit")
    rc, out, err = merge(env)
    assert rc == 5 and "renumber" in err
    assert not (repo / ".git" / "MERGE_HEAD").exists()


def test_dry_run_with_blocking_conflict_returns_5(env):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n"})
    write(repo, "a.txt", "main a\n")
    commit_all(repo, "main a")
    rc, out, err = merge(env, "--dry-run")
    assert rc == 5 and "blocking=1" in out


@pytest.mark.parametrize("deleted", [False, True])
def test_explicit_conflict_resolution(env, tmp_path, deleted):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n"})
    if deleted:
        git(repo, "rm", "a.txt")
        git(repo, "commit", "-q", "-m", "main deletion")
    else:
        write(repo, "a.txt", "main a\n")
        commit_all(repo, "main edits")
    result = tmp_path / "resolved.txt"
    result.write_bytes(b"combined\r\n")
    spec = "a.txt=" + ("-" if deleted else str(result))
    assert merge(env, "--dry-run", "--resolution", spec)[0] == 0
    rc, out, err = merge(env, "--resolution", spec)
    assert rc == 0, err
    assert len(parents(repo)) == 2
    assert not (repo / "a.txt").exists() if deleted else (repo / "a.txt").read_bytes() == b"combined\r\n"


@pytest.mark.parametrize("spec", ["../a.txt=-", "/a.txt=-", ".git/config=-", "b.txt=-"])
def test_invalid_resolution_does_not_start_merge(env, spec):
    repo, _ = env
    lane_branch(repo, {"a.txt": "lane a\n"})
    head = git(repo, "rev-parse", "HEAD")
    rc, _, _ = merge(env, "--resolution", spec)
    assert rc == 5
    assert git(repo, "rev-parse", "HEAD") == head


def _branch(repo, name, rel, content, subject):
    git(repo, "checkout", "-q", "-b", name, "main")
    write(repo, rel, content)
    commit_all(repo, subject)
    git(repo, "checkout", "-q", "main")


def test_batch_merges_each_tip_with_baselines_after_each_and_verifies_once(env, monkeypatch):
    repo, tools = env
    for n in "abc":
        _branch(repo, "lane_" + n, f"{n}_lane.cpp", n + "\n", f"lane {n}")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "file_size_ratchet.py", "tighten")
    lm = _load_module()
    verifies = []
    monkeypatch.setattr(lm, "verify_merged", lambda root, mode, runner=None: (verifies.append(mode), ("OK", []))[1])
    rc = lm.main(["--batch", "lane_a", "lane_b", "lane_c", "--root", str(repo), "--tools-dir", str(tools)])
    assert rc == 0
    assert verifies == ["build"]
    log = git(repo, "log", "--format=%s", "-8").splitlines()
    follow = "merge follow-up: ratchet baselines tightened at merged HEAD"
    assert log.index(follow) < log.index("merge lane: lane a") and log.index("merge lane: lane b") < log.index(follow)   # right after tip a, before tip b
    assert [s for s in log if s.startswith("merge lane:")] == ["merge lane: lane c", "merge lane: lane b", "merge lane: lane a"]
    assert all((repo / f"{n}_lane.cpp").exists() for n in "abc")


def test_batch_dry_run_plans_later_tips_against_the_earlier_ones(env, capsys):
    repo, tools = env
    _branch(repo, "lane_a", "a.txt", "from a\n", "lane a")
    _branch(repo, "lane_b", "a.txt", "from b\n", "lane b")      # conflicts with lane_a only once a is merged
    head = git(repo, "rev-parse", "HEAD")
    lm = _load_module()
    assert lm.main(["--batch", "lane_a", "lane_b", "--dry-run", "--root", str(repo), "--tools-dir", str(tools)]) == 5
    out = capsys.readouterr().out
    assert out.count("PLAN lane=") == 2 and "blocking=1" in out and "a.txt" in out
    assert git(repo, "rev-parse", "HEAD") == head and git(repo, "status", "--porcelain") == ""


def test_batch_needs_exactly_one_form(env):
    repo, tools = env
    lm = _load_module()
    with pytest.raises(SystemExit):
        lm.main(["lane", "--batch", "x", "--root", str(repo)])
    with pytest.raises(SystemExit):
        lm.main(["--batch", "x", "y", "--resolution", "p=-", "--root", str(repo)])


def _run_batch(env_, *argv):
    repo, tools = env_
    lm = _load_module()
    verifies = []
    lm.verify_merged = lambda root, m, runner=None: (verifies.append(m), ("OK", []))[1]
    rc = lm.main(["--batch", *argv, "--root", str(repo), "--tools-dir", str(tools)])
    return rc, verifies


def test_batch_message_is_a_template(env):
    repo, _ = env
    for n in "ab":
        _branch(repo, "lane_" + n, f"{n}_lane.txt", n + "\n", f"lane {n}")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    rc, _ = _run_batch(env, "lane_a", "lane_b", "--message", "w{n}: {subject}")
    assert rc == 0
    assert [s for s in git(repo, "log", "--format=%s", "-6").splitlines() if s.startswith("w")] == ["w2: lane b", "w1: lane a"]


def test_batch_dry_run_plans_past_a_conflicted_tip(env, capsys):
    repo, tools = env
    _branch(repo, "lane_a", "a.txt", "from a\n", "lane a")
    _branch(repo, "lane_b", "a.txt", "from b\n", "lane b")      # blocks once a is merged
    _branch(repo, "lane_c", "c.txt", "c\n", "lane c")
    lm = _load_module()
    assert lm.main(["--batch", "lane_a", "lane_b", "lane_c", "--dry-run", "--root", str(repo), "--tools-dir", str(tools)]) == 5
    out = capsys.readouterr().out
    assert out.count("PLAN lane=") == 3 and "approximate" in out


def test_batch_refuses_to_start_when_a_later_tip_cannot_merge(env, capsys):
    repo, tools = env
    _branch(repo, "lane_a", "a.txt", "from a\n", "lane a")
    _branch(repo, "lane_b", "a.txt", "from b\n", "lane b")
    head = git(repo, "rev-parse", "HEAD")
    lm = _load_module()
    assert lm.main(["--batch", "lane_a", "lane_b", "--root", str(repo), "--tools-dir", str(tools)]) == 5
    assert "BATCH REFUSED" in capsys.readouterr().err
    assert git(repo, "rev-parse", "HEAD") == head and git(repo, "status", "--porcelain") == ""


def test_batch_ratchet_failure_stops_and_names_the_rest(env, capsys):
    repo, _ = env
    for n in "abc":
        _branch(repo, "lane_" + n, f"{n}_lane.txt", n + "\n", f"lane {n}")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "cxx17_ratchet/cxx17_ratchet.py", "fail")
    rc, verifies = _run_batch(env, "lane_a", "lane_b", "lane_c")
    out = capsys.readouterr().out
    assert rc == 6 and verifies == []     # a tooling-only partial tree has nothing to build
    assert "BATCH STOPPED at lane_a merged=1 remaining=lane_b lane_c" in out
    last = out.strip().splitlines()[-1]
    assert last.startswith("MERGED ") and "ratchets=FAIL" in last and "SKIPPED(ratchet-failure)" in last
    assert (repo / "a_lane.txt").exists() and not (repo / "b_lane.txt").exists()


def test_batch_with_compiled_changes_skips_build_on_ratchet_failure(env, capsys):
    repo, _ = env
    _branch(repo, "lane_a", "src/a.cpp", "int a;\n", "lane a")
    _branch(repo, "lane_b", "b_lane.txt", "b\n", "lane b")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "cxx17_ratchet/cxx17_ratchet.py", "fail")
    rc, verifies = _run_batch(env, "lane_a", "lane_b")
    assert rc == 6 and verifies == []
    assert "remaining=lane_b" in capsys.readouterr().out


def test_compiled_input_classifier():
    lm = _load_module()
    for rel in ("a/b.cpp", "x.h", "shaders/a.frag", "CMakeLists.txt", "cmake/x.cmake", "Gates/rows.inc", "RenderCore/a.hglsl"):
        assert lm.COMPILED_INPUTS.search(rel), rel
    for rel in ("tools/a.py", "docs/a.md", "scripts/check-contracts.sh", "data/x.json"):
        assert not lm.COMPILED_INPUTS.search(rel), rel


def test_regeneration_reports_only_owned_outputs(env, monkeypatch, capsys):
    lm = _load_module()
    repo, _ = env
    write(repo, "generate.py", "open('artifact.json', 'w').write('new\\n')\nopen('decoy', 'w').write('no')\n")
    write(repo, "artifact.json", "old\n")
    commit_all(repo, "generator")
    monkeypatch.setattr(lm, "ARTIFACTS", ["artifact.json"])
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "generate.py", [])])
    assert lm.regenerate_all(str(repo)) == ["artifact.json"]
    assert "artifact.json" in capsys.readouterr().out
    with pytest.raises(lm.Stop, match="preexisting artifact edits"):
        lm.regenerate_all(str(repo))


def test_regeneration_normalizes_windows_json_without_touching_binary(env, monkeypatch):
    lm = _load_module()
    repo, _ = env
    write(repo, "generate.py", "open('artifact.json', 'wb').write(b'new\\r\\n')\n")
    write(repo, "artifact.json", "old\n")
    (repo / "artifact.spv").write_bytes(b"\r\n\x00")
    commit_all(repo, "generator")
    monkeypatch.setattr(lm, "ARTIFACTS", ["artifact.json", "artifact.spv"])
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "generate.py", [])])
    assert lm.regenerate_all(str(repo)) == ["artifact.json"]
    assert (repo / "artifact.json").read_bytes() == b"new\n"
    assert (repo / "artifact.spv").read_bytes() == b"\r\n\x00"


@pytest.mark.parametrize("mode", ["build", "none"])
def test_contained_batch_still_verifies(env, monkeypatch, mode):
    lm = _load_module()
    repo, tools = env
    verifies = []
    monkeypatch.setattr(lm, "verify_merged", lambda root, mode: (verifies.append(mode), ("OK", []))[1])
    monkeypatch.setattr(lm, "merge_tip", lambda *a: pytest.fail("contained tip merged"))
    assert lm.main(["--batch", "HEAD", "--verify", mode, "--root", str(repo), "--tools-dir", str(tools)]) == 0
    assert verifies == [mode]


def test_regeneration_failure_stops_before_later_generators(env, monkeypatch):
    lm = _load_module()
    repo, _ = env
    write(repo, "generate.py", "raise SystemExit('broken generator')\n")
    commit_all(repo, "generator")
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "generate.py", []), ("never", "missing.py", [])])
    with pytest.raises(lm.Stop, match="broken generator"):
        lm.regenerate_all(str(repo))


def test_dirty_source_cannot_contaminate_artifact_commit(env, monkeypatch):
    lm = _load_module()
    repo, tools = env
    write(repo, "generate.py", "from pathlib import Path\nPath('artifact.json').write_bytes(Path('b.txt').read_bytes())\n")
    write(repo, "artifact.json", "b\n")
    commit_all(repo, "generator")
    lane_branch(repo, {"a.txt": "lane\n"})
    monkeypatch.setattr(lm, "ARTIFACTS", ["artifact.json"])
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "generate.py", [])])
    before = git(repo, "rev-parse", "HEAD")
    write(repo, "b.txt", "uncommitted source must never enter artifacts\n")
    for action in (lambda: lm.regenerate_all(str(repo)),
                   lambda: lm.merge_tip(str(repo), str(tools), lm.plan(str(repo), "lane"), "merge lane", {})):
        with pytest.raises(lm.Stop, match="requires a clean tree.*"):
            action()
        assert git(repo, "rev-parse", "HEAD") == before
        assert (repo / "artifact.json").read_bytes() == b"b\n"
        assert "uncommitted source" in (repo / "b.txt").read_text()
    # Once the input is committed, only its generated artifact enters the follow-up.
    git(repo, "add", "--", "b.txt")
    git(repo, "commit", "-q", "-m", "commit generator input")
    _, follow, failed = lm.merge_tip(str(repo), str(tools), lm.plan(str(repo), "lane"), "merge lane", {})
    assert follow and not failed
    assert git(repo, "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD") == "artifact.json"
    assert "uncommitted source" in git(repo, "show", "HEAD:artifact.json")


def test_regeneration_dry_run_leaves_original_untouched(env, monkeypatch):
    lm = _load_module()
    repo, _ = env
    write(repo, "generate.py", "open('artifact.json', 'w').write('new\\n')\n")
    write(repo, "artifact.json", "old\n")
    commit_all(repo, "generator")
    monkeypatch.setattr(lm, "ARTIFACTS", ["artifact.json"])
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "generate.py", [])])
    assert lm.regenerate_all(str(repo), dry_run=True) == ["artifact.json"]
    assert (repo / "artifact.json").read_text() == "old\n"
    assert git(repo, "status", "--porcelain") == ""


def test_merge_commits_regenerated_artifacts_before_verification(env, monkeypatch):
    lm = _load_module()
    repo, tools = env
    write(repo, "generate.py", "open('artifact.json', 'w').write('new\\n')\nopen('decoy', 'w').write('no')\n")
    write(repo, "artifact.json", "old\n")
    commit_all(repo, "generator")
    lane_branch(repo, {"a.txt": "lane\n"})
    monkeypatch.setattr(lm, "ARTIFACTS", ["artifact.json"])
    monkeypatch.setattr(lm, "GENERATORS", [("fixture", "generate.py", [])])
    _, follow, failed = lm.merge_tip(str(repo), str(tools), lm.plan(str(repo), "lane"), "merge lane", {})
    assert follow and not failed
    assert git(repo, "log", "-1", "--format=%s") == "merge follow-up: regenerated artifacts"
    assert git(repo, "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD") == "artifact.json"
    assert git(repo, "status", "--porcelain") == "?? decoy"
