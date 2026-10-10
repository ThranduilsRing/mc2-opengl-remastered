"""tools/_git_util.py against a throwaway repository (no network, no project state)."""
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _git_util as gu  # noqa: E402

console_child = gu.console_child


def _run(root, *args):
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c", "user.name=t", *args], check=True,
                   capture_output=True, **console_child.child_kwargs(redirected=True))


@pytest.fixture()
def repo(tmp_path):
    _run(tmp_path, "init", "-q")
    (tmp_path / "a.txt").write_text("a\n")
    (tmp_path / "b.txt").write_text("b\n")
    _run(tmp_path, "add", "-A")
    _run(tmp_path, "commit", "-q", "-m", "init")
    return str(tmp_path)


def test_git_out_and_failure(repo):
    assert len(gu.git_out(repo, "rev-parse", "HEAD")) == 40
    with pytest.raises(gu.GitError):
        gu.git_out(repo, "rev-parse", "--verify", "no-such-ref")
    assert gu.is_repo(repo)


def test_status_entries_and_dirty_paths(repo):
    (open(os.path.join(repo, "a.txt"), "w")).write("changed\n")
    open(os.path.join(repo, "new.txt"), "w").write("n\n")
    entries = dict((p, xy) for xy, p in gu.status_entries(repo))
    assert entries["a.txt"].strip() == "M" and entries["new.txt"] == "??"
    assert gu.dirty_paths(repo) == {"a.txt", "new.txt"}
    assert gu.dirty_paths(repo, tracked_only=True) == {"a.txt"}


def test_rename_lists_both_paths(repo):
    _run(repo, "mv", "a.txt", "c.txt")
    paths = [p for _xy, p in gu.status_entries(repo)]
    assert "c.txt" in paths and "a.txt" in paths


def test_merge_in_progress_is_false_on_a_quiet_repo(repo):
    assert gu.merge_in_progress(repo) is False and gu.unmerged_paths(repo) == []


def test_commit_paths_commits_only_the_named_paths(repo):
    open(os.path.join(repo, "a.txt"), "w").write("changed\n")
    open(os.path.join(repo, "b.txt"), "w").write("changed\n")
    sha = gu.commit_paths(repo, ["a.txt"], "touch a", trailer="Co-Authored-By: T <t@t>")
    assert gu.git_out(repo, "rev-parse", "--short=9", "HEAD") == sha
    assert gu.git_out(repo, "show", "--name-only", "--format=", "HEAD").split() == ["a.txt"]
    assert "b.txt" in gu.dirty_paths(repo)


def test_commit_paths_refuses_when_something_else_is_staged(repo):
    open(os.path.join(repo, "a.txt"), "w").write("changed\n")
    open(os.path.join(repo, "b.txt"), "w").write("changed\n")
    _run(repo, "add", "b.txt")
    with pytest.raises(gu.GitError, match="swept in"):
        gu.commit_paths(repo, ["a.txt"], "touch a")
    assert gu.git_out(repo, "log", "--format=%s").splitlines() == ["init"]
