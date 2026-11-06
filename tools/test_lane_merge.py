"""Tests for tools/lane_merge.py (throwaway git repo + fake ratchet scripts; stdlib + git only).

Run:
  py -3 -B -m pytest tools/test_lane_merge.py -q -p no:cacheprovider
"""
import json
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
if mode == "tighten" and "--tighten" in sys.argv:
    if BASELINE:
        open(BASELINE, "w").write("tightened by " + name + "\\n")
    open("tools/decoy.json", "w").write("decoy " + name + "\\n")
sys.exit(0)
'''
FAKES = {
    "file_size_ratchet.py": FSIZE,
    "cxx17_ratchet/cxx17_ratchet.py": CXX,
    "axis_swizzle_ratchet.py": AXIS,
    "split/split_map.py": None,
}


def precheck_fixture(lm, repo, monkeypatch, red, tree=None):
    git(repo, "branch", "claude/nifty-mendeleev")
    actual_run = lm.run
    seen = []

    def runner(root, cmd, env=None):
        if cmd[0] == "git":
            return actual_run(root, cmd, env)
        if any(lm.Path(c).name == "ungated_log_tags.py" for c in cmd):
            return subprocess.CompletedProcess(cmd, 0, "")   # git-only stage, not part of the clone chains
        seen.append((str(root), cmd))
        return subprocess.CompletedProcess(cmd, int(any(lm.Path(a).name == red for a in cmd)), "fixture failure")
    monkeypatch.setattr(lm, "run", runner)
    if tree is not None:
        import bootstrap_worktree_build as bwb
        monkeypatch.setattr(bwb, "ensure_deps", lambda *a: None)
        monkeypatch.setattr(lm, "PRECHECK_TREE", str(tree))
    return seen


def is_native(cmd):
    return any(a in cmd for a in ("scripts/ninja_build.py", "scripts/configure_ninja.py"))


@pytest.mark.parametrize("red", [None, "callers.py", "run_scripts_tests.py", "run_tools_tests.py"])
def test_precheck_fast_path_has_no_native_build_and_no_scratch_in_root(env, monkeypatch, capsys, red):
    from tools import lane_merge as lm
    repo, _ = env
    before = git(repo, "rev-parse", "HEAD")
    seen = precheck_fixture(lm, repo, monkeypatch, red)
    assert lm.main(["--root", str(repo), "--precheck", "HEAD"]) == int(red is not None)
    assert len(seen) == 9    # 4 ratchets + callers --check + 2 early rows + 2 suites
    assert not any(is_native(cmd) for _, cmd in seen)
    assert all(not lm._inside(root, str(repo)) for root, _ in seen)   # scratch is never inside the main worktree
    caller_commands = [cmd for _, cmd in seen if "tools/callers.py" in cmd]
    assert caller_commands == [[sys.executable, "-B", "tools/callers.py", "--check"]]
    assert all("--tighten" not in cmd for _, cmd in seen)
    assert {label for label, _ in lm.PYTHON_SUITES} == {"scripts_tests", "tools_tests"}
    assert git(repo, "rev-parse", "HEAD") == before
    assert not git(repo, "status", "--porcelain")
    out = capsys.readouterr().out
    assert "precheck timing (s): virtual-merge=" in out and "clone0=" in out and "WALL=" in out
    assert "scripts_tests: " in out and "s)" in out
    assert ("PRECHECK FAIL" if red else "PRECHECK PASS conflicts=0 ratchets=5 link=skipped") in out


@pytest.mark.parametrize("red", [None, "configure_ninja.py", "ninja_build.py", "mc2fx"])
def test_precheck_with_link_uses_warm_tree_outside_root(env, monkeypatch, capsys, tmp_path, red):
    from tools import lane_merge as lm
    repo, _ = env
    tree = tmp_path / "warm" / "tree"
    seen = precheck_fixture(lm, repo, monkeypatch, red, tree)
    assert lm.main(["--root", str(repo), "--precheck", "HEAD", "--with-link"]) == int(red is not None)
    assert len(seen) == 12
    # only configure + link run in the warm tree; the checks run in shared clones
    build = [r for r, cmd in seen if any(x in " ".join(cmd) for x in ("configure_ninja.py", "ninja_build.py"))]
    assert build and all(root == str(tree) for root in build)
    assert not lm._inside(str(tree), str(repo))
    for _, cmd in seen:
        if "scripts/ninja_build.py" in cmd:
            assert "-j" + str(max(1, int(lm.host_jobs()) // 2)) in cmd   # precheck link stages take JOBS/2 beside the suites (precheck-speed)
    assert any("mc2fx" in cmd and "mc2fx_preview" in cmd for _, cmd in seen)
    assert (tree / ".git").exists()
    # second run reuses the existing tree (checkout, not worktree add)
    seen.clear()
    lm.main(["--root", str(repo), "--precheck", "HEAD", "--with-link"])
    assert seen and any(root == str(tree) for root, _ in seen)
    out = capsys.readouterr().out
    assert "link-mc2=" in out and "tree-checkout=" in out


def test_precheck_refuses_tree_or_scratch_inside_root(env, monkeypatch):
    from tools import lane_merge as lm
    repo, _ = env
    with pytest.raises(lm.Stop):
        lm.ensure_precheck_tree(str(repo), str(repo / "sub" / "tree"), "HEAD")
    monkeypatch.setattr(lm.tempfile, "gettempdir", lambda: str(repo / "tmp"))
    monkeypatch.setattr(lm.os.path, "isdir", lambda p: False)
    with pytest.raises(lm.Stop):
        lm.precheck_scratch_base(str(repo))


def test_tree_lock_is_exclusive(tmp_path):
    from tools import lane_merge as lm
    path = str(tmp_path / "x.lock")
    sleeps = []
    t = [0.0]
    with lm.TreeLock(path):
        with pytest.raises(lm.Stop):
            with lm.TreeLock(path, wait_s=10, sleep=lambda s: (sleeps.append(s), t.__setitem__(0, t[0] + 5)), clock=lambda: t[0]):
                pass
        assert sleeps
    with lm.TreeLock(path):   # released after the holder exits
        pass


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


@pytest.fixture(autouse=True)
def _no_receipts_by_default(monkeypatch):
    monkeypatch.setenv("MC2_NO_RECEIPT_CHECK", "1")   # the receipt tests below unset it
    # these tests grade THIS tree's lane_merge.py: without it, a copy outside mc2-main (a precheck clone) hands off to the trusted mc2-main copy
    monkeypatch.setenv("MC2_LANE_MERGE_TRUSTED", "1")


@pytest.fixture
def env(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    for rel in (FSIZE, CXX, AXIS, SHADERS):
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
    assert "line 5 of cxx17_ratchet" in err and "line 0 of cxx17_ratchet" in err  # a failing ratchet prints its whole tail (60 lines), not 3
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


def test_resolution_restores_deleted_directory_matching_gitignore(env, tmp_path):
    repo, _ = env
    path = "generated/only.txt"
    write(repo, path, "base\n")
    commit_all(repo, "nested base")
    lane_branch(repo, {path: "lane edit\n"})
    git(repo, "rm", path)
    write(repo, ".gitignore", "generated/\n")
    commit_all(repo, "delete directory and ignore it")
    assert not (repo / "generated").exists()
    result = tmp_path / "resolved.txt"
    result.write_bytes(b"resolved\r\n")
    rc, _, err = merge(env, "--resolution", f"{path}={result}")
    assert rc == 0, err
    assert (repo / path).read_bytes() == b"resolved\r\n"
    assert git(repo, "show", f"HEAD:{path}") == "resolved"
    assert len(parents(repo)) == 2


def _branch(repo, name, rel, content, subject):
    git(repo, "checkout", "-q", "-b", name, "main")
    write(repo, rel, content)
    commit_all(repo, subject)
    git(repo, "checkout", "-q", "main")


def test_batch_checks_each_tip_and_tightens_once_before_verify(env, monkeypatch):
    repo, tools = env
    for n in "abc":
        _branch(repo, "lane_" + n, f"{n}_lane.cpp", n + "\n", f"lane {n}")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "file_size_ratchet.py", "tighten")
    lm = _load_module()
    checks = []
    real_ratchets = lm.run_ratchets
    def ratchets(root, tools_dir, *, tighten=True):
        checks.append(tighten)
        return real_ratchets(root, tools_dir, tighten=tighten)
    monkeypatch.setattr(lm, 'run_ratchets', ratchets)
    verifies = []
    monkeypatch.setattr(lm, "verify_merged", lambda root, mode, runner=None: (verifies.append(mode), ("OK", []))[1])
    rc = lm.main(["--batch", "lane_a", "lane_b", "lane_c", "--root", str(repo), "--tools-dir", str(tools), "--no-receipt-check"])
    assert rc == 0
    assert verifies == ["build"]
    log = git(repo, "log", "--format=%s", "-8").splitlines()
    follow = "merge follow-up: ratchet baselines tightened at merged HEAD"
    assert log.count(follow) == 1 and log.index(follow) < log.index('merge lane: lane c')
    assert checks == [False, False, False, True]
    assert [s for s in log if s.startswith("merge lane:")] == ["merge lane: lane c", "merge lane: lane b", "merge lane: lane a"]
    assert all((repo / f"{n}_lane.cpp").exists() for n in "abc")


def test_batch_artifacts_and_tightening_share_one_followup(env, monkeypatch):
    repo, tools = env
    _branch(repo, 'lane_a', 'a_lane.txt', 'a\n', 'lane a')
    _branch(repo, 'lane_b', 'b_lane.txt', 'b\n', 'lane b')
    set_mode(env, 'file_size_ratchet.py', 'tighten')
    lm = _load_module()
    generated = []
    def regenerate(root):
        generated.append(root)
        write(repo, 'docs/split-map.md', '{"batch": true}\n')
        return ['docs/split-map.md']
    monkeypatch.setattr(lm, 'regenerate_all', regenerate)
    monkeypatch.setattr(lm, 'verify_merged', lambda *a: ('OK', []))
    assert lm.main(['--batch', 'lane_a', 'lane_b', '--root', str(repo), '--tools-dir', str(tools)]) == 0
    assert len(generated) == 1
    assert set(git(repo, 'show', '--format=', '--name-only', 'HEAD').splitlines()) == {FSIZE, 'docs/split-map.md'}
    assert git(repo, 'log', '-1', '--format=%s') == lm.FOLLOWUP_SUBJECT


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
    assert lm.main(["--batch", "lane_a", "lane_b", "--root", str(repo), "--tools-dir", str(tools), "--no-receipt-check"]) == 5
    assert "BATCH REFUSED" in capsys.readouterr().err
    assert git(repo, "rev-parse", "HEAD") == head and git(repo, "status", "--porcelain") == ""


def test_batch_ratchet_failure_keeps_merging_and_lists_all_reds(env, capsys):
    repo, _ = env
    for n in "abc":
        _branch(repo, "lane_" + n, f"{n}_lane.txt", n + "\n", f"lane {n}")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "cxx17_ratchet/cxx17_ratchet.py", "fail")
    rc, verifies = _run_batch(env, "lane_a", "lane_b", "lane_c")
    out = capsys.readouterr().out
    assert rc == 6 and verifies == []     # a tooling-only partial tree has nothing to build
    assert "BATCH RATCHET REDS in 3 of 3 tip(s), merged=3" in out
    assert all(f"  lane_{n}: cxx17(rc=" in out for n in "abc")
    last = out.strip().splitlines()[-1]
    assert last.startswith("MERGED ") and "ratchets=FAIL" in last and "SKIPPED(ratchet-failure)" in last
    assert all((repo / f"{n}_lane.txt").exists() for n in "abc")


def test_batch_with_compiled_changes_skips_build_on_ratchet_failure(env, capsys):
    repo, _ = env
    _branch(repo, "lane_a", "src/a.cpp", "int a;\n", "lane a")
    _branch(repo, "lane_b", "b_lane.txt", "b\n", "lane b")
    write(repo, "main.txt", "m\n")
    commit_all(repo, "main work")
    set_mode(env, "cxx17_ratchet/cxx17_ratchet.py", "fail")
    rc, verifies = _run_batch(env, "lane_a", "lane_b")
    assert rc == 6 and verifies == []
    assert "BATCH RATCHET REDS in 2 of 2 tip(s), merged=2" in capsys.readouterr().out


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


def test_batch_resolutions_belong_to_individual_tips(env, tmp_path):
    repo, tools = env
    base = git(repo, "rev-parse", "HEAD")
    git(repo, "switch", "-c", "one")
    write(repo, "a.txt", "one\n")
    commit_all(repo, "one")
    git(repo, "switch", "-c", "two", base)
    write(repo, "b.txt", "two\n")
    commit_all(repo, "two")
    git(repo, "switch", "main")
    write(repo, "a.txt", "main-a\n")
    write(repo, "b.txt", "main-b\n")
    commit_all(repo, "main conflicts")
    ra, rb = tmp_path / "resolved-a", tmp_path / "resolved-b"
    ra.write_text("reviewed-a\n")
    rb.write_text("reviewed-b\n")
    command = [sys.executable, "-B", TOOL, "--batch", "one", "two", "--root", str(repo),
               "--tools-dir", str(tools), "--no-verify", "--resolution", f"one:a.txt={ra}",
               "--resolution", f"two:b.txt={rb}"]
    dry = subprocess.run(command + ["--dry-run"], capture_output=True, text=True,
                         **console_child.child_kwargs(redirected=True))
    assert dry.returncode == 0, dry.stdout + dry.stderr
    result = subprocess.run(command, capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stdout + result.stderr
    assert (repo / "a.txt").read_text() == "reviewed-a\n"
    assert (repo / "b.txt").read_text() == "reviewed-b\n"


GENERATED_FILES = ("Gates/gate_table.json", "docs/flags.auto.yaml", "docs/background-launch-inventory.md", "docs/split-map.md")


def test_every_generated_file_has_a_regenerator():
    lm = _load_module()
    assert lm.GENERATED == set().union(*(files for files, _, _ in lm.GENERATED_REGENERATORS))


def test_dup_check_has_no_baseline_and_is_not_a_lane_merge_ratchet():
    # copy-paste is compared against the merge base by tools/dup_check.py (check-contracts row), not pinned in a file
    lm = _load_module()
    assert all(r[0] != 'dup_lines' for r in lm.RATCHETS)
    assert not any('dup_lines' in p for p in lm.TIGHTENED_BASELINES) and not any('dup_lines' in p for p in lm.REGENERABLE)


@pytest.mark.parametrize("path,script,args", [
    ("Gates/gate_table.json", "tools/gate_gen.py", ["--sync-verified"]),
    ("docs/background-launch-inventory.md", "scripts/check-background-launch.py", ["--write", "--quiet"]),
    ("tests/unit/log_tag_baseline.json", "tools/log_tag_census.py", ["--write"]),
])
def test_generated_conflict_regenerates_and_commits_all_outputs(env, monkeypatch, path, script, args):
    repo, _ = env
    lm = _load_module()
    authored = ("Gates/gate_table.cpp", "Gates/gate_rows_core.inc", "Gates/gate_rows_render.inc")
    outputs = (path, *authored) if script == "tools/gate_gen.py" else (path,)
    for rel in outputs:
        write(repo, rel, "base\n")
    write(repo, script, "# fake generator\n")
    commit_all(repo, "generator baseline")
    lane_branch(repo, {path: "lane\n"})
    write(repo, path, "main\n")
    commit_all(repo, "main output")
    calls = []

    def runner(root, cmd, env=None):
        calls.append(cmd)
        for rel in outputs:
            write(repo, rel, "regenerated\n")
        write(repo, "a.txt", "unrelated\n")
        return subprocess.CompletedProcess(cmd, 0, "")

    monkeypatch.setattr(lm, "run", runner)
    lm.do_merge(str(repo), "lane", "merge lane")
    assert calls == [[sys.executable, "-B", os.path.join(str(repo), script), *args]]
    for rel in outputs:
        assert git(repo, "show", "HEAD:" + rel) == "regenerated"
    assert git(repo, "show", "HEAD:a.txt") == "a"
    assert git(repo, "diff", "--name-only") == "a.txt"
    assert not git(repo, "diff", "--cached", "--name-only")
    for rel in authored:
        assert not lm.is_generated(rel)


def _generated_repo(repo):
    """A synthetic tree with the generated files the W83 batch conflicted on."""
    for rel in GENERATED_FILES:
        write(repo, rel, "base " + rel + "\n")
    write(repo, "Gates/gate_table.json", '{"version": 1, "count": 2, "gates": []}\n')
    write(repo, "tests/unit/test_gate.cpp", 'TEST_CASE("x") {\n    REQUIRE(n == 2);\n}\n')
    write(repo, "regen.py",
          "import pathlib\n"
          "for rel in %r:\n"
          "    p = pathlib.Path(rel)\n"
          "    p.parent.mkdir(parents=True, exist_ok=True)\n"
          "    p.write_text('regenerated ' + rel + chr(10))\n" % (GENERATED_FILES,))
    commit_all(repo, "generated baseline")


def _branch_from(repo, name, base, edits, subject):
    git(repo, "checkout", "-q", "-b", name, base)
    for rel, content in edits.items():
        write(repo, rel, content)
    commit_all(repo, subject)
    git(repo, "checkout", "-q", "main")


def test_batch_auto_resolves_generated_conflicts_and_regenerates(env, monkeypatch, capsys):
    repo, tools = env
    _generated_repo(repo)
    base = git(repo, "rev-parse", "HEAD")
    _branch_from(repo, "lane_a", base, {"docs/flags.auto.yaml": "lane flags\n"}, "lane a")
    _branch_from(repo, "lane_b", base, {"docs/background-launch-inventory.md": "lane callers\n"}, "lane b")
    write(repo, "docs/flags.auto.yaml", "main flags\n")
    write(repo, "docs/background-launch-inventory.md", "main callers\n")
    commit_all(repo, "main generated")
    lm = _load_module()
    ran = []
    real_run = lm.run
    def runner(root, cmd, env=None):
        ran.append(os.path.basename(cmd[2]))
        return real_run(root, cmd, env)
    verifies = []
    monkeypatch.setattr(lm, "run", runner)
    monkeypatch.setattr(lm, "verify_merged", lambda root, mode, runner=None: (verifies.append(mode), ("OK", []))[1])
    monkeypatch.setattr(lm, "GENERATED_REGENERATORS", ((set(GENERATED_FILES), "regen.py", []),))
    rc = lm.main(["--batch", "lane_a", "lane_b", "--root", str(repo), "--tools-dir", str(tools), "--no-receipt-check"])
    assert rc == 0, capsys.readouterr().err
    assert ran.count("regen.py") == 2
    assert verifies == ["build"]
    for rel in ("docs/flags.auto.yaml", "docs/background-launch-inventory.md"):
        assert (repo / rel).read_text() == "regenerated " + rel + "\n"
    assert git(repo, "status", "--porcelain") == ""
    assert [s for s in git(repo, "log", "--format=%s", "-4").splitlines() if s.startswith("merge lane:")] == [
        "merge lane: lane b", "merge lane: lane a"]


def test_batch_still_stops_on_a_non_generated_conflict(env, monkeypatch, capsys):
    repo, tools = env
    _generated_repo(repo)
    base = git(repo, "rev-parse", "HEAD")
    _branch_from(repo, "lane_a", base, {"a.txt": "lane a\n", "docs/split-map.md": "lane map\n"}, "lane a")
    _branch_from(repo, "lane_b", base, {"docs/background-launch-inventory.md": "lane callers\n"}, "lane b")
    write(repo, "a.txt", "main a\n")
    write(repo, "docs/split-map.md", "main map\n")
    commit_all(repo, "main generated")
    lm = _load_module()
    monkeypatch.setattr(lm, "GENERATED_REGENERATORS", ((set(GENERATED_FILES), "regen.py", []),))
    rc, verifies = _run_batch(env, "lane_a", "lane_b")
    assert rc == 5 and verifies == []
    assert "BATCH REFUSED" in capsys.readouterr().err
    assert git(repo, "status", "--porcelain") == ""


def test_test_gate_floor_conflict_alone_is_auto_resolved(env, monkeypatch, capsys):
    repo, tools = env
    _generated_repo(repo)
    base = git(repo, "rev-parse", "HEAD")
    _branch_from(repo, "lane", base, {"tests/unit/test_gate.cpp":
                                      'TEST_CASE("x") {\n    REQUIRE(n == 5);\n}\n'}, "lane gate")
    write(repo, "tests/unit/test_gate.cpp", 'TEST_CASE("x") {\n    REQUIRE(n == 3);\n}\n')
    commit_all(repo, "main gate")
    lm = _load_module()
    monkeypatch.setattr(lm, "GENERATED_REGENERATORS", ())
    rc, out, err = merge(env, "--no-verify")
    assert rc == 0, err
    assert "REQUIRE(n == 2);" in (repo / "tests/unit/test_gate.cpp").read_text()


def test_test_gate_conflict_with_body_change_still_aborts(env):
    repo, _ = env
    _generated_repo(repo)
    base = git(repo, "rev-parse", "HEAD")
    _branch_from(repo, "lane", base, {"tests/unit/test_gate.cpp":
                                      'TEST_CASE("x") {\n    REQUIRE(n == 2);\n    CHECK(lane());\n}\n'}, "lane body")
    write(repo, "tests/unit/test_gate.cpp", 'TEST_CASE("x") {\n    REQUIRE(n == 2);\n    CHECK(main());\n}\n')
    commit_all(repo, "main body")
    rc, _, err = merge(env, "--no-verify")
    assert rc == 5 and "test_gate.cpp" in err


def test_floor_sync_preserves_crlf(tmp_path):
    lm = _load_module()
    (tmp_path / "Gates").mkdir()
    (tmp_path / "tests/unit").mkdir(parents=True)
    (tmp_path / "Gates/gate_table.json").write_text('{"count": 9}\n', encoding="utf-8")
    (tmp_path / "tests/unit/test_gate.cpp").write_bytes(b'TEST_CASE("x") {\r\n    REQUIRE(n == 1);\r\n}\r\n')
    assert lm.sync_test_gate_floor(str(tmp_path))
    assert (tmp_path / "tests/unit/test_gate.cpp").read_bytes() == b'TEST_CASE("x") {\r\n    REQUIRE(n == 9);\r\n}\r\n'


# ---- batch-end derived inventories: tighten-only (gate floor, log tag census) ----

LOGTAGS = "tests/unit/log_tag_baseline.json"


def _tags(*rows, consumed=()):
    return {"emitters": [{"tag": t, "ver": v, "stream": "stderr", "keys": ["a"], "count": n} for t, v, n in rows],
            "consumed_tags": list(consumed), "plain_tags": []}


def test_log_tag_census_accepts_additions_only():
    lm = _load_module()
    old = _tags(("A", 1, 2), consumed=["A"])
    assert lm.log_tag_loosened(old, _tags(("A", 1, 2), ("B", 1, 1), consumed=["A", "B"])) == []
    assert lm.log_tag_loosened(old, _tags(("A", 1, 3), consumed=["A"])) == []


def test_log_tag_census_loosening_is_reported():
    lm = _load_module()
    old = _tags(("A", 1, 2), ("B", 1, 1), consumed=["A"])
    assert any("B v1" in w for w in lm.log_tag_loosened(old, _tags(("A", 1, 2))))
    assert any("A v1" in w for w in lm.log_tag_loosened(old, _tags(("A", 1, 1), ("B", 1, 1))))
    assert any("consumed_tags" in w for w in lm.log_tag_loosened(old, _tags(("A", 1, 2), ("B", 1, 1))))


def test_gate_floor_loosening_needs_retirement():
    lm = _load_module()
    assert lm.gate_floor_loosened({"A", "B"}, {"A", "B", "C"}, set()) == []
    assert lm.gate_floor_loosened({"A", "B"}, {"A"}, {"B"}) == []
    assert lm.gate_floor_loosened({"A", "B"}, {"A"}, set()) == ["B"]


def _gate_table(names):
    rows = ", ".join('{"name": "%s"}' % n for n in names)
    return '{"count": %d, "gates": [%s]}\n' % (len(names), rows)


def _gate_repo(env, names, floor):
    repo, _ = env
    write(repo, "Gates/gate_table.json", _gate_table(names))
    write(repo, "Gates/retired_names.json", '["OLD"]\n')
    write(repo, "tests/unit/test_gate.cpp", "    REQUIRE(n == %d);   // floor\n" % floor)
    write(repo, LOGTAGS, json.dumps(_tags(("A", 1, 1))))
    commit_all(repo, "gates")


def _derived_batch(env, monkeypatch, lane_edits, regen):
    repo, tools = env
    git(repo, "checkout", "-q", "-b", "lane_g", "main")
    for rel, content in lane_edits.items():
        write(repo, rel, content)
    commit_all(repo, "lane gates")
    git(repo, "checkout", "-q", "main")
    lm = _load_module()
    monkeypatch.setattr(lm, "regenerate_all", lambda root: regen(repo) or [])
    monkeypatch.setattr(lm, "verify_merged", lambda *a: ("OK", []))
    return lm, lm.main(["--batch", "lane_g", "--root", str(repo), "--tools-dir", str(tools)])


def test_batch_derived_inventories_commit_once_and_only_tighten(env, monkeypatch):
    _gate_repo(env, ["A", "B"], 2)
    repo, _ = env
    lm, rc = _derived_batch(env, monkeypatch, {"Gates/gate_table.json": _gate_table(["A", "B", "C"])},
                            lambda r: (write(r, LOGTAGS, json.dumps(_tags(("A", 1, 1), ("NEW", 1, 1)))), [LOGTAGS])[1])
    assert rc == 0
    assert git(repo, "log", "-1", "--format=%s") == lm.DERIVED_SUBJECT
    assert set(git(repo, "show", "--format=", "--name-only", "HEAD").splitlines()) == {"tests/unit/test_gate.cpp", LOGTAGS}
    assert "REQUIRE(n == 3);" in (repo / "tests/unit/test_gate.cpp").read_text(encoding="utf-8")


def test_batch_fails_loudly_when_a_gate_is_lost_without_retirement(env, monkeypatch, capsys):
    _gate_repo(env, ["A", "B"], 2)
    repo, _ = env
    lm, rc = _derived_batch(env, monkeypatch, {"Gates/gate_table.json": _gate_table(["A"])}, lambda r: None)
    assert rc == 6
    assert "not in Gates/retired_names.json" in capsys.readouterr().err
    assert "REQUIRE(n == 2);" in (repo / "tests/unit/test_gate.cpp").read_text(encoding="utf-8")


def test_batch_fails_loudly_and_restores_a_loosened_log_tag_census(env, monkeypatch, capsys):
    _gate_repo(env, ["A"], 1)
    repo, _ = env
    lm, rc = _derived_batch(env, monkeypatch, {"x.txt": "x\n"}, lambda r: write(r, LOGTAGS, json.dumps(_tags())))
    assert rc == 6
    assert "log tag census loosened" in capsys.readouterr().err
    assert json.loads((repo / LOGTAGS).read_text(encoding="utf-8")) == _tags(("A", 1, 1))


# ---- early contract rows + exe fingerprint guard (W109 lessons) ----

def test_early_rows_are_the_contract_commands():
    lm = _load_module()
    assert dict((l, (s, a)) for l, s, a in lm.EARLY_ROWS) == {
        "sim_include_firewall": ("scripts/check_sim_include_firewall.py", []),
        "log_tag_census": ("tools/log_tag_census.py", ["--check"])}


def test_precheck_runs_the_early_rows(env, monkeypatch):
    repo, _ = env
    lm = _load_module()
    seen = precheck_fixture(lm, repo, monkeypatch, "check_sim_include_firewall.py")
    assert lm.main(["--root", str(repo), "--precheck", "HEAD"]) == 1     # the firewall row is red -> precheck is red
    names = [lm.Path(a).name for _, cmd in seen for a in cmd]
    assert "check_sim_include_firewall.py" in names
    assert any(cmd[-2:] == ["tools/log_tag_census.py", "--check"] for _, cmd in seen)


def test_pre_gate_runs_the_early_rows(env, monkeypatch, capsys):
    repo, _ = env
    lm = _load_module()
    cmds = []
    monkeypatch.setattr(lm, "regenerate_all", lambda root, dry_run=False: [])
    monkeypatch.setattr(lm, "exe_matches_head", lambda root, tree="build64-ninja": None)

    def runner(root, cmd, env=None):
        cmds.append(cmd)
        return subprocess.CompletedProcess(cmd, int(any("log_tag_census" in str(c) for c in cmd)), "tag census red")
    monkeypatch.setattr(lm, "run", runner)
    assert lm.pre_gate(str(repo), exe=str(repo / "no.exe")) == 1
    out = capsys.readouterr().out
    assert "pre-gate: sim_include_firewall: OK" in out
    assert "pre-gate: log_tag_census: FAIL" in out
    assert "PRE-GATE FAIL: log_tag_census" in out


def test_pre_gate_fails_on_a_stale_exe_fingerprint(env, monkeypatch, capsys):
    repo, _ = env
    lm = _load_module()
    exe = repo / "build64-ninja" / "mc2.exe"
    exe.parent.mkdir()
    exe.write_bytes(b"x")
    monkeypatch.setattr(lm, "regenerate_all", lambda root, dry_run=False: [])
    monkeypatch.setattr(lm, "exe_matches_head", lambda root, tree="build64-ninja": False)
    monkeypatch.setattr(lm, "run", lambda root, cmd, env=None: subprocess.CompletedProcess(cmd, 0, ""))
    assert lm.pre_gate(str(repo), exe=str(exe)) == 1
    assert "PRE-GATE FAIL: exe-fingerprint" in capsys.readouterr().out


def test_rebuild_if_head_moved_skips_a_matching_or_missing_exe(env):
    repo, _ = env
    lm = _load_module()
    for state in (True, None):
        ran = []
        ok, _ = lm.rebuild_if_head_moved(str(repo), check=lambda r, state=state: state,
                                         runner=lambda *a, **k: ran.append(a))
        assert ok and not ran


def test_rebuild_if_head_moved_rebuilds_then_accepts(env):
    repo, _ = env
    lm = _load_module()
    states = iter([False, True])
    ran = []
    ok, msg = lm.rebuild_if_head_moved(str(repo), check=lambda r: next(states),
                                       runner=lambda root, cmd, env=None: ran.append(cmd) or subprocess.CompletedProcess(cmd, 0, ""))
    assert ok and not msg
    assert len(ran) == 1 and ran[0][1].replace("\\", "/").endswith("scripts/ninja_build.py")


def test_rebuild_if_head_moved_fails_loudly(env):
    repo, _ = env
    lm = _load_module()
    ok, msg = lm.rebuild_if_head_moved(str(repo), check=lambda r: False,
                                       runner=lambda root, cmd, env=None: subprocess.CompletedProcess(cmd, 1, "link error"))
    assert not ok and "rebuild before the gate failed" in msg
    ok, msg = lm.rebuild_if_head_moved(str(repo), check=lambda r: False,
                                       runner=lambda root, cmd, env=None: subprocess.CompletedProcess(cmd, 0, ""))
    assert not ok and "still differs" in msg


def test_batch_refuses_tip_without_precheck_receipt_and_accepts_after_pass(env, tmp_path, monkeypatch, capsys):
    repo, tools = env
    monkeypatch.delenv("MC2_NO_RECEIPT_CHECK")
    _branch(repo, "lane_a", "a.txt", "from a\n", "lane a")
    head = git(repo, "rev-parse", "HEAD")
    lm = _load_module()
    monkeypatch.setattr(lm, "PRECHECK_RECEIPTS", str(tmp_path / "receipts"))
    sha = git(repo, "rev-parse", "lane_a")
    assert lm.main(["--batch", "lane_a", "--root", str(repo), "--tools-dir", str(tools)]) == 5
    out = capsys.readouterr()
    assert f"PRECHECK MISSING {sha}" in out.out and git(repo, "rev-parse", "HEAD") == head
    lm.write_receipt(str(repo), "lane_a", False, "PASS")
    assert lm.missing_receipts(str(repo), ["lane_a"]) == []
    lm.write_receipt(str(repo), "lane_a", False, "FAIL")
    assert lm.missing_receipts(str(repo), ["lane_a"]) == [sha]       # only a PASS receipt counts
    (tmp_path / "receipts" / (sha + ".json")).write_text("{")
    assert lm.missing_receipts(str(repo), ["lane_a"]) == [sha]       # corrupt receipt counts as missing


def test_stage_merges_without_precheck_receipts_but_batch_still_refuses(env, tmp_path, monkeypatch, capsys):
    # the staged candidate's gate is the proof (--promote needs a complete PASS verdict), so --stage must not demand per-tip receipts
    repo, tools = env
    monkeypatch.delenv("MC2_NO_RECEIPT_CHECK")
    _branch(repo, "lane_a", "a.txt", "from a\n", "lane a")
    git(repo, "branch", "claude/nifty-mendeleev", "main")
    lm = _load_module()
    monkeypatch.setattr(lm, "PRECHECK_RECEIPTS", str(tmp_path / "receipts"))
    monkeypatch.setattr(lm, "stage_preflight", lambda root, window: [])   # the fixture repo has no merge_gate.py
    assert lm.main(["--batch", "lane_a", "--root", str(repo), "--tools-dir", str(tools)]) == 5    # plain --batch: unchanged
    assert "PRECHECK MISSING" in capsys.readouterr().out
    rc = lm.main(["--stage", "W1", "--batch", "lane_a", "--root", str(repo), "--tools-dir", str(tools)])
    out = capsys.readouterr().out
    assert rc == 0 and "STAGE claude/stage-W1" in out and "PRECHECK MISSING" not in out
    assert git(repo, "branch", "--show-current") == "claude/stage-W1"
    assert git(repo, "show", "HEAD:a.txt") == "from a"


# ---- G3: sim/save-sensitive paths make --precheck require and run saveload + determinism rows ----

def test_sim_path_classification():
    from tools import lane_merge as lm
    hit = lm.sim_sensitive_paths(["mclib/mech3d_update.cpp", "mclib/gvactor.h", "mclib/msl_shape.cpp", "mclib/appear.h",
                                  "mclib/terrain_runtime.cpp", "code/mech_value_codec.h", "code/mission_update.cpp",
                                  "code/saveload_load.cpp", "GameOS/gameos/sim_overlap.cpp", "code/tick_commit.cpp"])
    assert len(hit) == 10
    miss = ["mclib/txmmgr.cpp", "GameOS/gameos/gameos_graphics.cpp", "scripts/merge_gate.py", "docs/x.md", "shaders/terrain.frag",
            "code/mech_ui.cpp", "tools/lane_merge.py"]
    assert lm.sim_sensitive_paths(miss) == []
    assert lm.sim_sensitive_paths(["a/b.cpp"], ["a/*"]) == ["a/b.cpp"]
    assert lm.SIM_ROWS == ("saveload-mc2_01", "saveload-mc2_10", "determinism-mc2_10")


def sim_tip(repo, tmp_path, monkeypatch, rel="mclib/mech3d.cpp"):
    """A tip touching `rel`, a fake install deployed from it, receipts in tmp."""
    from tools import lane_merge as lm
    precheck_fixture(lm, repo, monkeypatch, None)
    _branch(repo, "lane_s", rel, "x\n", "sim lane")
    sha = git(repo, "rev-parse", "lane_s")
    install = tmp_path / "inst"
    install.mkdir()
    (install / "mc2.exe").write_bytes(b"x")
    (install / ".deployed_manifest.csv").write_text(
        "manifest_version,v1,,,,\nrelpath,sha256,bytes,src_commit,timestamp,mtime_ns\nmc2.exe,0,1," + sha + ",t,1\n")
    monkeypatch.setattr(lm, "PRECHECK_RECEIPTS", str(tmp_path / "receipts"))
    return lm, sha, str(install)


def test_precheck_runs_the_sim_rows_with_the_gates_commands_and_records_them(env, monkeypatch, tmp_path, capsys):
    repo, _ = env
    lm, sha, install = sim_tip(repo, tmp_path, monkeypatch)
    ran = []

    def runner(root, cmd, env=None):
        ran.append(cmd)
        return subprocess.CompletedProcess(cmd, 0, "")
    assert lm.precheck(str(repo), "lane_s", False, sim_install=install, sim_runner=runner, sim_quiet=lambda: None) == 0
    flat = [" ".join(c) for c in ran]
    assert len(ran) == 3
    assert all(install in f for f in flat)
    sl = [f for f in flat if "saveload_roundtrip.py check" in f]
    assert len(sl) == 2 and all("--mask tools/save_noise_mask.json" in f for f in sl)
    assert any("--mission mc2_01" in f for f in sl) and any("--mission mc2_10" in f for f in sl)
    assert any("sim_determinism.py check mc2_10" in f for f in flat)
    rec = json.loads((tmp_path / "receipts" / (sha + ".json")).read_text())
    assert rec["verdict"] == "PASS" and rec["sim_rows"]["required"] and not rec["sim_rows"]["override"]
    assert rec["sim_rows"]["paths"] == ["mclib/mech3d.cpp"] and set(rec["sim_rows"]["rows"].values()) == {"PASS"}
    assert lm.missing_receipts(str(repo), ["lane_s"]) == []


def test_a_red_sim_row_fails_the_precheck_with_the_first_differing_line(env, monkeypatch, tmp_path, capsys):
    repo, _ = env
    lm, sha, install = sim_tip(repo, tmp_path, monkeypatch)

    def runner(root, cmd, env=None):
        j = " ".join(cmd)
        bad = "--mission mc2_10" in j and "saveload" in j
        return subprocess.CompletedProcess(cmd, 1 if bad else 0, "noise\nsave differs at packet 2 offset 77\nmore" if bad else "")
    assert lm.precheck(str(repo), "lane_s", False, sim_install=install, sim_runner=runner, sim_quiet=lambda: None) == 1
    out = capsys.readouterr().out
    assert "PRECHECK FAIL" in out and "saveload-mc2_10: FAIL: save differs at packet 2 offset 77" in out
    assert not (tmp_path / "receipts" / (sha + ".json")).exists()


@pytest.mark.parametrize("case", ["quiet", "no_install", "stale", "lease_rc"])
def test_infra_problems_are_not_run_and_block_the_receipt(env, monkeypatch, tmp_path, capsys, case):
    repo, _ = env
    lm, sha, install = sim_tip(repo, tmp_path, monkeypatch)
    quiet = (lambda: "GATE QUIET: gate pid 1") if case == "quiet" else (lambda: None)
    if case == "no_install":
        install = str(tmp_path / "nope")
    if case == "stale":
        import lane_sim_rows as lsr   # the module lane_merge itself imported
        monkeypatch.setattr(lsr, "stale_install", lambda r, i, s: "install exe was built from another commit")
    ran = []

    def runner(root, cmd, env=None):
        ran.append(cmd)
        return subprocess.CompletedProcess(cmd, 75 if case == "lease_rc" else 0, "GATE QUIET: refusing game run")
    assert lm.precheck(str(repo), "lane_s", False, sim_install=install, sim_runner=runner, sim_quiet=quiet) == 1
    out = capsys.readouterr().out
    assert "NOT RUN" in out and "PRECHECK FAIL" in out and "PRECHECK PASS" not in out
    if case != "lease_rc":
        assert ran == []
    assert not (tmp_path / "receipts" / (sha + ".json")).exists()
    assert lm.missing_receipts(str(repo), ["lane_s"]) == [sha]


def test_no_sim_rows_is_an_explicit_recorded_override(env, monkeypatch, tmp_path):
    repo, _ = env
    lm, sha, install = sim_tip(repo, tmp_path, monkeypatch)
    assert lm.precheck(str(repo), "lane_s", False, sim_rows=False, sim_runner=lambda *a, **k: 1 / 0) == 0
    rec = json.loads((tmp_path / "receipts" / (sha + ".json")).read_text())
    assert rec["sim_rows"]["override"] is True and rec["sim_rows"]["required"] and rec["sim_rows"]["rows"] == {}
    assert lm.missing_receipts(str(repo), ["lane_s"]) == []


def test_a_tip_without_sim_paths_runs_no_sim_rows_and_an_old_receipt_cannot_vouch_for_one(env, monkeypatch, tmp_path):
    repo, _ = env
    lm, sha, install = sim_tip(repo, tmp_path, monkeypatch, rel="docs/note.md")
    assert lm.precheck(str(repo), "lane_s", False, sim_runner=lambda *a, **k: 1 / 0) == 0
    assert json.loads((tmp_path / "receipts" / (sha + ".json")).read_text())["sim_rows"]["required"] is False
    _branch(repo, "lane_t", "mclib/gvactor.cpp", "y\n", "sim lane 2")
    lm.write_receipt(str(repo), "lane_t", False, "PASS")
    assert lm.missing_receipts(str(repo), ["lane_t"]) == [git(repo, "rev-parse", "lane_t")]


def test_receipt_path_refuses_a_non_40_hex_sha_and_tests_use_a_private_dir():
    from tools import lane_merge as lm
    for bad in ("abc", "", "g" * 40, "a" * 39):
        with pytest.raises(ValueError):
            lm.receipt_path(bad)
    assert lm.receipt_path("a" * 40).endswith("a" * 40 + ".json")
    assert not os.path.normpath(lm.PRECHECK_RECEIPTS).startswith(os.path.normpath("A:/Games/mc2-ops"))


def test_ensure_precheck_tree_recreates_tree_that_lost_its_git(env, monkeypatch, tmp_path):
    from tools import lane_merge as lm
    import bootstrap_worktree_build as bwb
    repo, _ = env
    monkeypatch.setattr(bwb, "ensure_deps", lambda *a, **k: None)
    tree = tmp_path / "warm" / "tree"
    lm.ensure_precheck_tree(str(repo), str(tree), "HEAD")
    assert (tree / ".git").exists()
    (tree / ".git").unlink()                      # lose the link, keep leftover build files
    (tree / "build64-ninja").mkdir()
    lm.ensure_precheck_tree(str(repo), str(tree), "HEAD")
    assert (tree / ".git").exists() and not (tree / "build64-ninja").exists()


# --- merge queue v1: planted semantic violation, staging promotion/refusal, bisect -------------------------------------------

PLANTED = 'import os, sys\nsys.exit(1 if os.path.exists("violation.cpp") else 0)\n'


@pytest.mark.parametrize("batch", [True, False])
def test_planted_semantic_violation_stays_red_and_is_never_regenerated(env, monkeypatch, batch):
    """A merge that adds a pointer-order site fails against the COMMITTED baseline; regenerate_all (which would rewrite the
    baseline with --write-baseline and bless it) never runs."""
    repo, tools = env
    write(tools, "pointer_order_scan.py", PLANTED)
    _branch(repo, "lane_a", "violation.cpp", "std::map<T*, int> m;\n", "lane a")
    lm = _load_module()
    regenerated = []
    monkeypatch.setattr(lm, "regenerate_all", lambda *a, **k: regenerated.append(a) or [])
    monkeypatch.setattr(lm, "verify_merged", lambda *a, **k: ("OK", []))
    argv = (["--batch", "lane_a"] if batch else ["lane_a"]) + ["--root", str(repo), "--tools-dir", str(tools)]
    assert lm.main(argv) == 6
    assert regenerated == []


def _stage_fixture(env, monkeypatch):
    repo, _ = env
    lm = _load_module()
    import lane_merge_stage
    monkeypatch.setattr(lane_merge_stage, "NIFTY", "main")
    write(repo, ".gitignore", "build64-ninja/\n")   # ignored in the real tree too
    commit_all(repo, "ignore build dir")
    _branch(repo, "lane_a", "a_lane.txt", "a\n", "lane a")
    old = git(repo, "rev-parse", "main")
    lm.stage(str(repo), "t1", ["lane_a"])
    assert git(repo, "branch", "--show-current") == "claude/stage-t1"
    git(repo, "merge", "-q", "--no-ff", "-m", "merge lane: lane a", "lane_a")
    cand = git(repo, "rev-parse", "HEAD")
    state = json.loads((repo / "build64-ninja" / "stage-t1.json").read_text(encoding="utf-8"))
    assert state["base"] == old and state["testing"] == [git(repo, "rev-parse", "lane_a")]
    return lm, repo, old, cand


def _verdict(repo, **kw):
    data = dict(schema=1, head=None, verdict="PASS", complete=True, skipped={}) | kw
    path = repo / "build64-ninja" / "gate-verdict.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


@pytest.mark.parametrize("bad", [dict(verdict="FAIL", complete=False), dict(verdict="PARTIAL", complete=False),
                                 dict(skipped={"mods": "mods off"}), dict(head="0" * 40)])
def test_promote_refuses_and_leaves_nifty_untouched(env, monkeypatch, bad):
    lm, repo, old, cand = _stage_fixture(env, monkeypatch)
    _verdict(repo, **({"head": cand} | bad))
    with pytest.raises(lm.Stop) as e:
        lm.promote(str(repo), "t1")
    assert e.value.code == 6 and "nifty untouched" in str(e.value)
    assert git(repo, "rev-parse", "main") == old


def test_promote_refuses_without_a_verdict(env, monkeypatch):
    lm, repo, old, _ = _stage_fixture(env, monkeypatch)
    assert lm.main(["--promote", "t1", "--root", str(repo)]) == 6
    assert git(repo, "rev-parse", "main") == old


def test_promote_fast_forwards_on_a_complete_pass(env, monkeypatch):
    lm, repo, old, cand = _stage_fixture(env, monkeypatch)
    path = _verdict(repo, head=cand)
    assert lm.promote(str(repo), "t1") == 0
    assert git(repo, "rev-parse", "main") == cand and git(repo, "branch", "--show-current") == "main"
    assert "promotion" in json.loads(path.read_text(encoding="utf-8"))["timings"]


def test_bisect_halves_a_red_batch_and_rejects_the_guilty_tip():
    bisect_step = _load_module().bisect_step
    s = dict(testing=["a", "b", "c"], queue=["d"])
    s, act = bisect_step(s, passed=False)
    assert act == "retest" and s["testing"] == ["a", "b"] and s["queue"] == ["c", "d"]
    s, act = bisect_step(s, passed=True)
    assert act == "promote" and s["landed"] == ["a", "b"] and s["testing"] == ["c", "d"] and s["queue"] == []
    s, act = bisect_step(s, passed=False)
    assert act == "retest" and s["testing"] == ["c"] and s["queue"] == ["d"]
    s, act = bisect_step(s, passed=False)
    assert act == "guilty" and s["rejected"] == ["c"] and s["testing"] == ["d"]
    assert len(s["history"]) == 4


def test_stage_help_documents_the_queue_commands(capsys):
    with pytest.raises(SystemExit):
        _load_module().main(["--help"])
    out = capsys.readouterr().out
    assert all(k in out for k in ("--stage W --batch", "--promote W", "--bisect W --red", "nifty untouched"))

