#!/usr/bin/env python3
"""Merge a lane tip into the current branch (nifty) and re-tighten the ratchets, fail-closed.

WHY: lane merges follow the same ceremony every time -- check the lane does not collide
with uncommitted work in this shared worktree, merge --no-ff, regenerate the ratchet
baselines that always conflict, re-run the ratchets at the merged HEAD, and commit only
the baselines that tightened. Doing it by hand risks sweeping another lane's dirty or
staged files into the merge commit.

Usage:
  py -3 tools/lane_merge.py <sha-or-branch> [--message "..."] [--dry-run]
  --message   merge commit subject (default: "merge lane: <subject of the lane tip>")
  --dry-run   print the plan (base, file counts, overlap, predicted conflicts); change nothing
  --root DIR  repo to operate on (default: this checkout)
  --tools-dir DIR  where the ratchet scripts live (default: <root>/tools); same relative layout

Steps: (a) dirty-overlap and staged-index refusal, (b) git merge --no-ff with regenerable
baseline conflicts resolved to the lane's side, (c) ratchet checks with --tighten at the merged
HEAD, (d) a follow-up commit of exactly the tightened baseline files.

Exit codes: 0 merged (or lane already contained); 2 bad ref; 3 merge already in progress or
unmerged paths; 4 dirty overlap / staged changes; 5 merge failed or conflicts outside the
regenerable set (merge aborted, the lane's untracked files removed; a "file in use" failure waits up to 10 min for
build-alltargets/.tidy.lock to clear and retries once); 6 a ratchet failed (merge commit stays).
Final line: MERGED <sha> (+<follow-up sha>) files=<n> ratchets=OK/FAIL overlap=0
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)
sys.path.insert(0, HERE)
import _git_util  # noqa: E402  (shared git helpers)

TRAILER = "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
FOLLOWUP_SUBJECT = "merge follow-up: ratchet baselines tightened at merged HEAD"

# Files that are pure functions of the tree: on conflict the lane's copy is taken and the
# ratchet --tighten run below regenerates them against the merged result.
REGENERABLE = {
    "tools/file_size_ratchet.baseline.json",
    "tools/cxx17_ratchet/baseline.json",
    "tools/dup_lines_ratchet.baseline.json",
    "tools/axis_swizzle_ratchet.baseline.json",
    "scripts/gl_shader_variants.json",
}
# The baselines the follow-up commit may contain (never a glob over tools/).
TIGHTENED_BASELINES = [
    "tools/file_size_ratchet.baseline.json",
    "tools/cxx17_ratchet/baseline.json",
    "tools/dup_lines_ratchet.baseline.json",
    "tools/axis_swizzle_ratchet.baseline.json",
]
# (label, script relative to the tools dir, args)
RATCHETS = [
    ("file_size", "file_size_ratchet.py", ["--check", "--tighten"]),
    ("cxx17", "cxx17_ratchet/cxx17_ratchet.py", ["--check", "--tighten"]),
    ("dup_lines", "dup_lines_ratchet.py", ["--check", "--tighten"]),
    ("axis_swizzle", "axis_swizzle_ratchet.py", ["--check", "--tighten"]),
    ("split_map", "split/split_map.py", ["--check"]),
]
UNMERGED = {"DD", "AU", "UD", "UA", "DU", "AA", "UU"}


class Stop(Exception):
    def __init__(self, code: int, msg: str):
        super().__init__(msg)
        self.code = code


def run(root: str, cmd: list[str]) -> subprocess.CompletedProcess:  # non-git commands (the ratchets)
    return subprocess.run(cmd, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          encoding="utf-8", errors="replace", **console_child.child_kwargs(redirected=True))


def git(root: str, *args: str) -> subprocess.CompletedProcess:
    return _git_util.git(root, *args, merge_stderr=True)


def git_out(root: str, *args: str) -> str:
    r = git(root, *args)
    if r.returncode != 0:
        raise Stop(2, f"git {' '.join(args)} failed: {r.stdout.strip()}")
    return r.stdout.strip()


def lines(text: str) -> list[str]:
    return [ln for ln in text.splitlines() if ln]


def status_entries(root: str) -> list[tuple[str, str]]:
    return _git_util.status_entries(root)


def merge_in_progress(root: str) -> bool:
    return _git_util.merge_in_progress(root)


def plan(root: str, ref: str) -> dict:
    """Facts that decide whether the merge may proceed. Raises Stop on a refusal."""
    sha = git_out(root, "rev-parse", "--verify", "--quiet", ref + "^{commit}")
    if merge_in_progress(root):
        raise Stop(3, "a merge is already in progress; finish or abort it first")
    entries = status_entries(root)
    unmerged = sorted({p for xy, p in entries if xy in UNMERGED})
    if unmerged:
        raise Stop(3, "unmerged paths present:\n  " + "\n  ".join(unmerged))
    staged = lines(git_out(root, "diff", "--cached", "--name-only"))
    if staged:
        raise Stop(4, "index has staged changes that the merge commit would sweep in:\n  " + "\n  ".join(staged))
    head = git_out(root, "rev-parse", "HEAD")
    contained = git(root, "merge-base", "--is-ancestor", sha, "HEAD").returncode == 0
    base = git_out(root, "merge-base", "HEAD", sha) if not contained else sha
    lane_files = lines(git_out(root, "diff", "--name-only", base, sha))
    nifty_files = lines(git_out(root, "diff", "--name-only", base, head))
    dirty = {p for _, p in entries}
    overlap = sorted(set(lane_files) & dirty)
    return {"sha": sha, "head": head, "base": base, "contained": contained, "lane_files": lane_files,
            "nifty_files": nifty_files, "overlap": overlap}


def predicted_conflicts(root: str, sha: str) -> list[str] | None:
    """Conflicted paths a real merge would hit (None when git cannot predict)."""
    r = git(root, "merge-tree", "--write-tree", "--name-only", "--no-messages", "HEAD", sha)
    if r.returncode == 0:
        return []
    if r.returncode == 1:
        out = lines(r.stdout)
        return out[1:] if out else []  # first line is the result tree
    return None


def tip_subject(root: str, sha: str) -> str:
    return git_out(root, "log", "-1", "--format=%s", sha)


#: git's wording when a file in the worktree cannot be replaced because another process holds it (a tidy build, a running game).
TRANSIENT = re.compile(r"unable to unlink|unable to create file|permission denied|cannot unlink|invalid argument|being used by another",
                       re.IGNORECASE)
TIDY_LOCK = os.path.join("build-alltargets", ".tidy.lock")


def abort_and_clean(root: str, lane_files) -> list[str]:
    """Undo a failed merge: abort it if it started, then delete the UNTRACKED files the lane introduced.

    WHY: a merge that dies part-way (a file in the worktree is locked) leaves the lane's new files written but untracked, and the retry then
    refuses them as dirty overlap. Only untracked paths that are in the lane's change set are removed; any other untracked file is kept."""
    if merge_in_progress(root):
        git(root, "merge", "--abort")
    lane = set(lane_files)
    removed = []
    for xy, path in status_entries(root):
        if xy == "??" and path in lane:
            try:
                os.remove(os.path.join(root, path))
                removed.append(path)
            except OSError:
                pass
    return removed


def wait_for_lock(lock_path: str, wait_s: float, sleep=time.sleep, clock=time.monotonic) -> bool:
    """True when `lock_path` existed and has cleared within wait_s; False when there was nothing to wait for or it never cleared."""
    if not os.path.exists(lock_path):
        return False
    deadline = clock() + wait_s
    while os.path.exists(lock_path):
        if clock() >= deadline:
            return False
        sleep(10)
    return True


def do_merge(root: str, sha: str, message: str, lane_files=(), lock_path: str | None = None, lock_wait_s: float = 600,
             sleep=time.sleep, clock=time.monotonic) -> None:
    """Merge --no-ff; auto-resolve regenerable conflicts to the lane's side, abort on anything else.

    A failure that is not a conflict is cleaned up (see abort_and_clean). When git says a file could not be replaced and a tidy run holds
    the build tree, wait up to lock_wait_s for its lock to clear and retry once."""
    lock_path = lock_path or os.path.join(root, TIDY_LOCK)
    for attempt in (1, 2):
        r = git(root, "merge", "--no-ff", "-m", message, "-m", TRAILER, sha)
        if r.returncode == 0:
            return
        if merge_in_progress(root):
            break
        removed = abort_and_clean(root, lane_files)
        note = ("; removed the lane's untracked files: " + ", ".join(removed)) if removed else ""
        if attempt == 1 and TRANSIENT.search(r.stdout) and wait_for_lock(lock_path, lock_wait_s, sleep, clock):
            print("lane_merge: a file was in use and the tidy lock has cleared; retrying the merge once", file=sys.stderr)
            continue
        raise Stop(5, "merge failed before it started" + note + ":\n" + r.stdout.strip())
    conflicted = lines(git(root, "diff", "--name-only", "--diff-filter=U").stdout)
    foreign = [p for p in conflicted if p not in REGENERABLE]
    if foreign or not conflicted:
        abort_and_clean(root, lane_files)
        what = foreign or ["(merge failed without conflicts)"]
        raise Stop(5, "conflicts outside the regenerable set; merge aborted:\n  " + "\n  ".join(what)
                   + ("" if foreign else "\n" + r.stdout.strip()))
    for p in conflicted:
        c = git(root, "checkout", "--theirs", "--", p)
        a = git(root, "add", "--", p) if c.returncode == 0 else c
        if c.returncode != 0 or a.returncode != 0:
            abort_and_clean(root, lane_files)
            raise Stop(5, f"could not take the lane's {p}; merge aborted: {(c.stdout + a.stdout).strip()}")
    c = git(root, "commit", "-m", message, "-m", TRAILER)
    if c.returncode != 0:
        abort_and_clean(root, lane_files)
        raise Stop(5, "merge commit failed; merge aborted: " + c.stdout.strip())


def run_ratchets(root: str, tools_dir: str) -> list[tuple[str, int, str]]:
    results = []
    for label, rel, args in RATCHETS:
        script = os.path.join(tools_dir, *rel.split("/"))
        r = run(root, [sys.executable, "-B", script, *args])
        tail = "\n".join(lines(r.stdout)[-3:])
        results.append((label, r.returncode, tail))
    return results


def commit_tightened(root: str, pre_dirty: set[str]) -> str | None:
    """Commit exactly the baselines the ratchets tightened; skip any that was already dirty."""
    now_dirty = {p for _, p in status_entries(root)}
    changed = [p for p in TIGHTENED_BASELINES if p in now_dirty and p not in pre_dirty]
    skipped = [p for p in TIGHTENED_BASELINES if p in now_dirty and p in pre_dirty]
    if skipped:
        print("lane_merge: baseline already had uncommitted edits, left alone: " + ", ".join(skipped),
              file=sys.stderr)
    if not changed:
        return None
    try:
        return _git_util.commit_paths(root, changed, FOLLOWUP_SUBJECT, TRAILER)
    except _git_util.GitError as e:
        raise Stop(5, "follow-up commit failed: " + str(e))


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", help="lane tip: sha or branch")
    ap.add_argument("--message")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--root", default=os.path.abspath(os.path.join(HERE, "..")))
    ap.add_argument("--tools-dir")
    return ap


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    args = build_parser().parse_args(argv)
    root = args.root
    tools_dir = args.tools_dir or os.path.join(root, "tools")
    try:
        p = plan(root, args.ref)
        message = args.message or f"merge lane: {tip_subject(root, p['sha'])}"
        if args.dry_run:
            print(f"PLAN lane={p['sha'][:9]} head={p['head'][:9]} base={p['base'][:9]} "
                  f"lane_files={len(p['lane_files'])} nifty_files={len(p['nifty_files'])} "
                  f"overlap={len(p['overlap'])}")
            if p["contained"]:
                print("PLAN lane tip is already contained in HEAD: nothing to merge")
            elif p["overlap"]:
                raise Stop(4, "lane files are also dirty in the worktree:\n  " + "\n  ".join(p["overlap"]))
            else:
                conf = predicted_conflicts(root, p["sha"])
                if conf is None:
                    print("PLAN conflicts: unknown (git merge-tree unavailable)")
                else:
                    regen = [c for c in conf if c in REGENERABLE]
                    other = [c for c in conf if c not in REGENERABLE]
                    print(f"PLAN conflicts={len(conf)} regenerable={len(regen)} blocking={len(other)}"
                          + ("" if not other else " (merge would abort): " + ", ".join(other)))
                print(f'PLAN merge --no-ff -m "{message}"')
            print("PLAN then: " + ", ".join(label for label, _, _ in RATCHETS)
                  + "; follow-up commit of tightened baselines only")
            print("[dry-run] nothing changed")
            return 0
        if p["contained"]:
            print(f"MERGED {p['head'][:9]} lane tip already contained files=0 ratchets=SKIPPED overlap=0")
            return 0
        if p["overlap"]:
            raise Stop(4, "lane files are also dirty in the worktree; no merge attempted:\n  "
                       + "\n  ".join(p["overlap"]))
        pre_dirty = {path for _, path in status_entries(root)}
        do_merge(root, p["sha"], message, lane_files=p["lane_files"])
        merge_sha = git_out(root, "rev-parse", "--short=9", "HEAD")
        results = run_ratchets(root, tools_dir)
        follow = commit_tightened(root, pre_dirty)
        failed = [(label, rc, tail) for label, rc, tail in results if rc != 0]
        line = (f"MERGED {merge_sha}" + (f" (+{follow})" if follow else "")
                + f" files={len(p['lane_files'])} ratchets={'FAIL' if failed else 'OK'} overlap=0")
        print(line)
        if failed:
            for label, rc, tail in failed:
                print(f"lane_merge: ratchet {label} exited {rc}; last lines:\n{tail}", file=sys.stderr)
            return 6
        return 0
    except Stop as e:
        print(f"lane_merge: {e}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
