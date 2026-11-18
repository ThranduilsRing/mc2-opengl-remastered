"""Warm precheck tree and failure-summary helpers split out of lane_merge.py (imported there)."""
from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, HERE)
import _git_util  # noqa: E402

# Keep local branch names as well as origin/*: board checks resolve their bases
# by name, including in a base-rot clone made from another scratch clone.
SCRATCH_CLONE_ARGS = ("clone", "--quiet", "--shared", "--no-checkout", "--config",
                      "remote.origin.fetch=+refs/heads/*:refs/heads/*")


def git(root: str, *args: str):
    return _git_util.git(root, *args, merge_stderr=True)


class Stop(Exception):
    def __init__(self, code: int, msg: str):
        super().__init__(msg)
        self.code = code


def failure_line(line):
    """Keep pytest/doctest names and source locations even before a long tail."""
    return bool(re.search(r"FAILED|FAIL:|ERROR|VIOLATION|PERF_COUNTER_WARN|test case:|TEST CASE:|\.(?:py|cpp|h)[:(]\d+", line))


def failure_summary(output):
    rows = output.splitlines()
    return "\n".join(dict.fromkeys([line for line in rows if failure_line(line)] + rows[-8:]))


def failure_signatures(output):
    """Compare diagnostics, keeping symbols/counts but ignoring checkout roots and line shifts."""
    rows = output.splitlines()
    pytest = [row.strip() for row in rows if re.match(r"^(FAILED|ERROR) \S+", row)]
    if pytest:
        nodes = [row.split()[1] for row in pytest]
        pytest += [row.strip() for row in rows if row.startswith(("FAIL: ", "VIOLATION: "))
                   and not any(node in row for node in nodes)]
        return Counter(pytest)
    diagnostics = []
    for row in rows:
        row = re.sub(r"\b[0-9a-f]{9,40}\b", "<sha>", row.strip())
        row = re.sub(r"([.](?:py|cpp|h))[:(]\d+\)?", r"\1:<line>", row)
        row = re.sub(r"\b\d+(?:\.\d+)?\s*(?:s|seconds)\b", "<time>", row)
        if row and not re.search(r"^=|^collected ", row):
            diagnostics.append(row)
    return Counter(diagnostics)


def base_rot(root, base, label, command, result, runner, env=None):
    """Reproduce a red on the merge-base; unknown or additional diagnostics stay red.

    Print the base SHA, command and failure receipt so a warning never silently blesses rot.
    Native builds and diff-based checks are deliberately not eligible.
    """
    if not result.returncode or label in {"configure", "link-mc2", "link-tools", "ungated_tags", "new_drive_paths"}:
        return False
    if not re.search(r"\w+/[^\s:]+\.\w+|\[[A-Z][A-Z_0-9]*(?: v\d+)?\]|^(?:FAILED|ERROR) .+::", result.stdout, re.M):
        return False  # no file/symbol diagnostic: cannot attribute this failure safely
    with tempfile.TemporaryDirectory(prefix="lane-base-", dir=precheck_scratch_base(root)) as scratch:
        clone = git(root, *SCRATCH_CLONE_ARGS, os.path.abspath(root), scratch)
        if clone.returncode or git(scratch, "checkout", "--quiet", "--detach", base).returncode:
            return False
        cmd = [os.path.join(scratch, os.path.relpath(arg, root))
               if os.path.isabs(arg) and _inside(arg, root) else arg for arg in command]
        old = runner(scratch, cmd, env=env) if env is not None else runner(scratch, cmd)
        current = result.stdout.replace(root, "<tree>")
        previous = old.stdout.replace(scratch, "<tree>")
        signatures = failure_signatures(current)
        if not old.returncode or not signatures:
            return False
        unmatched = signatures - failure_signatures(previous)
        print(f"BASE-ROT {label} base={base} command={' '.join(command)} receipt:\n{failure_summary(old.stdout)}", flush=True)
        if unmatched:
            result.stdout = "\n".join(unmatched.elements())
        return not unmatched


PRECHECK_TREE = os.environ.get("MC2_PRECHECK_TREE", "E:/mc2-installs/_precheck/tree")


def _inside(path: str, root: str) -> bool:
    path, root = os.path.abspath(path), os.path.abspath(root)
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:   # different drives
        return False


def precheck_scratch_base(root: str) -> str | None:
    """Scratch parent for the fast path: G:/mc2-tmp (lane-rules), else the OS temp dir. Never inside the main worktree."""
    base = "G:/mc2-tmp" if os.name == "nt" and os.path.isdir("G:/mc2-tmp") else None
    if _inside(base or tempfile.gettempdir(), root):
        raise Stop(5, f"precheck scratch {base or tempfile.gettempdir()} is inside {root}")
    return base


class TreeLock:
    """Exclusive OS file lock (the OS drops it if the holder dies); two prechecks never share the warm tree."""
    def __init__(self, path, wait_s=3600, sleep=time.sleep, clock=time.monotonic):
        self.path, self.wait_s, self.sleep, self.clock, self.fh = path, wait_s, sleep, clock, None

    def _try(self):
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(self.fh.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(self.fh, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def __enter__(self):
        if os.environ.get("MC2_TEST_NO_TREELOCK") == "1" and os.environ.get("PYTEST_CURRENT_TEST"):   # pytest children only; the gate never sets both
            return self
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.fh = open(self.path, "a+")
        deadline = self.clock() + self.wait_s
        announced = False
        while True:
            try:
                self._try()
                return self
            except OSError:
                if self.clock() >= deadline:
                    self.fh.close()
                    raise Stop(5, "precheck tree busy: lock " + self.path + " held past wait")
                if not announced:
                    print("precheck: warm tree busy (another --with-link run); waiting", flush=True)
                    announced = True
                self.sleep(5)

    def __exit__(self, *exc):
        if self.fh is None:
            return
        self.fh.close()   # closing drops the OS lock


def ensure_precheck_tree(root: str, tree: str, commit: str) -> None:
    """Create the persistent warm worktree once (detached, outside the main worktree), then park it on `commit`."""
    if _inside(tree, root):
        raise Stop(5, f"warm precheck tree {tree} is inside {root}")
    if not os.path.exists(os.path.join(tree, ".git")):
        if os.path.isdir(tree) and os.listdir(tree):
            # Lost its .git (leftover build/deps only): drop the dead registration and the husk, then recreate below.
            print(f"[lane_merge] warm precheck tree {tree} has no .git; recreating it", flush=True)
            git(root, "worktree", "prune")
            shutil.rmtree(tree, ignore_errors=True)
            if os.path.isdir(tree) and os.listdir(tree):
                raise Stop(5, f"warm precheck tree {tree} has no .git and cannot be removed; delete it by hand, then "
                              f"`git -C {root} worktree prune` and rerun")
        os.makedirs(os.path.dirname(os.path.abspath(tree)), exist_ok=True)
        r = git(root, "worktree", "add", "--detach", "--force", os.path.abspath(tree), commit)
        if r.returncode:
            raise Stop(5, "cannot create warm tree: " + failure_summary(r.stdout))
        from bootstrap_worktree_build import ensure_deps
        ensure_deps(os.path.abspath(tree), os.path.abspath(root))
    else:
        r = git(tree, "checkout", "--quiet", "-f", "--detach", commit)
        if r.returncode:
            raise Stop(5, "warm tree checkout failed: " + failure_summary(r.stdout)
                       + f" (recreate with: delete {tree}, `git -C {root} worktree prune`, rerun)")
