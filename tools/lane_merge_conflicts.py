"""Conflict preview for `lane_merge.py --batch ... --dry-run --conflict-dir DIR` (read-only: no worktree, index or ref changes).

For every tip, in batch order, the caller has already planned a virtual merge against the running virtual head. This module writes
DIR/<NN>-<tipname>/<path> (the merge-tree result with conflict markers) for each conflicted path plus summary.txt, and tracks which
earlier tip touched the same file. The same-file pairs are a HEURISTIC (same path edited by two tips, generated files excluded):
two tips fixing the same thing in one file (W138f merge_gate empty-diff) show up here, but so do unrelated edits.
"""
from __future__ import annotations

import os
import re
import subprocess


def _git(root: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", root, *args], capture_output=True)


def tip_dirname(index: int, ref: str) -> str:
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", ref).strip("_") or "tip"
    return f"{index:02d}-{name}"


def blob_text(root: str, tree: str, path: str) -> bytes | None:
    r = _git(root, "show", f"{tree}:{path}")
    return r.stdout if r.returncode == 0 else None


def hunk_count(data: bytes | None) -> int:
    return 0 if data is None else len(re.findall(rb"^<<<<<<< ", data, re.M))


class ConflictPreview:
    def __init__(self, root: str, out_dir: str, base_head: str = "HEAD", is_noise=lambda path: False):
        self.root, self.out_dir, self.base_head, self.is_noise = root, out_dir, base_head, is_noise
        self.touched: list[tuple[str, set[str]]] = []   # (tip label, files it changes vs the original head)
        self.total_conflicts = 0
        self.tips_with_conflicts = 0
        self.pairs: list[tuple[str, str, str]] = []     # (file, earlier tip, later tip)

    def tip_files(self, sha: str) -> set[str]:
        base = _git(self.root, "merge-base", self.base_head, sha).stdout.decode().strip() or sha
        r = _git(self.root, "diff", "--name-only", base, sha)
        return {f for f in r.stdout.decode("utf-8", "replace").splitlines() if f and not self.is_noise(f)}

    def add_tip(self, index: int, ref: str, sha: str, tree: str | None, conflicted: list[str] | None) -> dict:
        label = tip_dirname(index, ref)
        files = self.tip_files(sha)
        same: dict[str, list[str]] = {}
        for earlier, efiles in self.touched:
            for f in sorted(files & efiles):
                same.setdefault(f, []).append(earlier)
                self.pairs.append((f, earlier, label))
        self.touched.append((label, files))
        unknown = conflicted is None
        conflicted = list(conflicted or [])
        hunks = {}
        tip_dir = os.path.join(self.out_dir, label)
        os.makedirs(tip_dir, exist_ok=True)
        for path in conflicted:
            data = blob_text(self.root, tree, path) if tree else None
            hunks[path] = hunk_count(data)
            dest = os.path.join(tip_dir, *path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fh:
                fh.write(data if data is not None else b"(no merged blob: delete/modify or rename conflict; see git merge-tree)\n")
        lines = [f"tip: {ref}", f"sha: {sha}", f"position in batch: {index}"]
        if unknown:
            lines.append("conflicts: unknown (git merge-tree unavailable)")
        lines.append(f"conflicted paths: {len(conflicted)}")
        for path in conflicted:
            earlier = same.get(path)
            lines.append(f"  {path}  hunks={hunks[path]}  also touched by earlier tip(s): "
                         + (", ".join(earlier) if earlier else "none (conflict is with nifty)"))
        shared = {f: e for f, e in same.items() if f not in conflicted}
        lines.append(f"same file, both tips (heuristic, not a conflict): {len(shared)}")
        for f, e in sorted(shared.items()):
            lines.append(f"  {f}  <- also in {', '.join(e)}")
        with open(os.path.join(tip_dir, "summary.txt"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
        if conflicted:
            self.tips_with_conflicts += 1
            self.total_conflicts += len(conflicted)
        return {"dir": tip_dir, "conflicted": conflicted, "shared": shared}

    def one_line(self, n_tips: int) -> str:
        return (f"CONFLICT-PREVIEW tips={n_tips} tips_with_conflicts={self.tips_with_conflicts} conflicted_paths={self.total_conflicts} "
                f"same_file_pairs={len(self.pairs)} dir={self.out_dir}")
