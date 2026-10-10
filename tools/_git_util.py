"""Shared git helpers for the orchestrator tools (handoff_append, lane_merge) and the tidy tools.

WHAT: run git without a console window and without losing output, read the merge state, parse `git status`, and commit exactly the paths
named (never the whole index).
WHY: each tool carried its own copy of these, with small differences (stderr merged or not, rename handling, how a merge in progress was
detected). A commit that sweeps in someone else's staged change has happened twice (see ORCHESTRATOR-HANDOFF); `commit_paths` is the one
place that rule is written down.
"""
from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)

#: porcelain XY codes of an unmerged path
UNMERGED = frozenset({"DD", "AU", "UD", "UA", "DU", "AA", "UU"})


class GitError(RuntimeError):
    pass


def git(root: str, *args: str, merge_stderr: bool = False) -> subprocess.CompletedProcess:
    """`git -C root args` as text (UTF-8, undecodable bytes replaced). stderr is separate unless merge_stderr."""
    return subprocess.run(["git", "-C", root, *args], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT if merge_stderr else subprocess.PIPE,
                          text=True, encoding="utf-8", errors="replace", **console_child.child_kwargs(redirected=True))


def git_out(root: str, *args: str) -> str:
    """stdout of a git command that must succeed (stripped); GitError carries the failure text."""
    r = git(root, *args, merge_stderr=True)
    if r.returncode != 0:
        raise GitError(f"git {' '.join(args)} failed: {r.stdout.strip()}")
    return r.stdout.strip()


def merge_in_progress(root: str) -> bool:
    p = git(root, "rev-parse", "--git-path", "MERGE_HEAD").stdout.strip()
    return bool(p) and os.path.exists(p if os.path.isabs(p) else os.path.join(root, p))


def is_repo(root: str) -> bool:
    return git(root, "rev-parse", "--git-dir").returncode == 0


def status_entries(root: str, untracked: str = "all") -> list[tuple[str, str]]:
    """(XY, path) for every worktree entry; a rename or copy lists both of its paths."""
    raw = git(root, "status", "--porcelain", "-z", f"--untracked-files={untracked}").stdout
    parts = raw.split("\0")
    out: list[tuple[str, str]] = []
    i = 0
    while i < len(parts):
        e = parts[i]
        i += 1
        if len(e) < 4:
            continue
        xy, path = e[:2], e[3:]
        out.append((xy, path))
        if "R" in xy or "C" in xy:
            if i < len(parts) and parts[i]:
                out.append((xy, parts[i]))
            i += 1
    return out


def unmerged_paths(root: str) -> list[str]:
    return [p for xy, p in status_entries(root) if xy in UNMERGED]


def dirty_paths(root: str, tracked_only: bool = False) -> set[str]:
    """Repo-relative paths with any uncommitted state (untracked included unless tracked_only)."""
    return {p for xy, p in status_entries(root, "no" if tracked_only else "all")}


def staged_paths(root: str) -> list[str]:
    return [ln for ln in git_out(root, "diff", "--cached", "--name-only").splitlines() if ln]


def commit_paths(root: str, paths: list[str], message: str, trailer: str | None = None) -> str:
    """Stage `paths` and commit ONLY them (`git commit -- paths`), returning the short sha.

    Refuses to run when anything outside `paths` is staged: `git commit` takes the whole index, and a parallel agent's staged change would
    otherwise ride along."""
    if not paths:
        raise GitError("commit_paths: no paths")
    other = [p for p in staged_paths(root) if p not in {q.replace("\\", "/") for q in paths}]
    if other:
        raise GitError("commit_paths: other paths are staged and would be swept in: " + ", ".join(other[:5]))
    a = git(root, "add", "--", *paths, merge_stderr=True)
    if a.returncode != 0:
        raise GitError("git add failed: " + a.stdout.strip())
    msg = ["-m", message] + (["-m", trailer] if trailer else [])
    c = git(root, "commit", *msg, "--", *paths, merge_stderr=True)
    if c.returncode != 0:
        raise GitError("git commit failed: " + c.stdout.strip())
    return git_out(root, "rev-parse", "--short=9", "HEAD")
