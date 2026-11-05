#!/usr/bin/env python3
"""Merge a lane tip into the current branch (nifty) and re-tighten the ratchets, fail-closed.

WHY: lane merges follow the same ceremony every time -- check the lane does not collide
with uncommitted work in this shared worktree, merge --no-ff, regenerate the ratchet
baselines that always conflict, re-run the ratchets at the merged HEAD, and commit only
the baselines that tightened. Doing it by hand risks sweeping another lane's dirty or
staged files into the merge commit.

Usage:
  py -3 tools/lane_merge.py --precheck <tip> [--with-link]
      fast early filter (minutes): virtual nifty merge conflicts, 5 ratchets, callers refresh+check, Python suites,
      per-stage seconds table; runs in a disposable clone under G:/mc2-tmp, never inside the main worktree.
      No native build: --batch --verify full and the gate still build everything. --with-link additionally links
      mc2, mc2fx, mc2fx_preview incrementally in a persistent warm worktree ($MC2_PRECHECK_TREE, default
      E:/mc2-installs/_precheck/tree, exclusive lock; the first run is a one-time cold build).
  py -3 tools/lane_merge.py <sha-or-branch> [--message "..."] [--dry-run]
  py -3 tools/lane_merge.py --batch A B C [--dry-run]   check each tip, tighten/regenerate once after the batch,
                                                        the build + doctest verify (step e) once at the end
  --message   merge commit subject (default: "merge lane: <subject of the lane tip>"); with --batch a template with {subject} and {n}
  --trailer   commit trailer line (default $MC2_COMMIT_TRAILER (or $MC2_MERGE_TRAILER), else none)
  --dry-run   print the plan (base, file counts, overlap, predicted conflicts); change nothing
  --resolution PATH=FILE  reviewed conflict result; in --batch use TIP:PATH=FILE (FILE - deletes; repeatable)
  --root DIR  repo to operate on (default: this checkout)
  --tools-dir DIR  where the ratchet scripts live (default: <root>/tools); same relative layout

Steps: (a) dirty-overlap and staged-index refusal, (b) git merge --no-ff with regenerable
baseline conflicts resolved to the lane's side, regenerate-all and one artifact follow-up commit,
(c) ratchet checks with --tighten at the merged
HEAD, (d) a follow-up commit of exactly the tightened baseline files.

Step (e), on by default: build the merged tree (scripts/ninja_build.py) and run the doctest tier (scripts/run-unit-tests.sh); `--verify full`
adds the whole contracts board, `--no-verify` skips it. A root without those scripts (the throwaway repos of the tests) reports SKIPPED.

Exit codes (a dry run that predicts a blocking conflict also exits 5; the BUGS table is merged mechanically, keep both and renumber): 0 merged (or lane already contained); 2 bad ref; 3 merge already in progress or
unmerged paths; 4 dirty overlap / staged changes; 5 merge failed or conflicts outside the
regenerable set (merge aborted, the lane's untracked files removed; a "file in use" failure waits up to 10 min for
build-alltargets/.tidy.lock to clear and retries once); 6 a ratchet failed (merge commit stays).
7 the merged tree does not build or its doctest fails (the merge commit stays; fix forward).
Final line: MERGED <sha> (+<follow-up sha>) files=<n> ratchets=OK/FAIL verify=OK/FAIL/SKIPPED overlap=0
A --batch run first plans every tip (a tip that would abort refuses the whole batch, exit 5, nothing merged); a ratchet failure mid-batch does not
stop the batch: every tip is merged, then "BATCH RATCHET REDS" lists all failing ratchets per tip, the build is skipped and it exits 6.
Each tip runs checks without tightening; a green batch regenerates and tightens once,
commits all owned outputs together, then builds every shipped executable at that final HEAD.
Every successful merged batch verifies, including tooling-only tips, so the embedded SHA is HEAD.
A --batch run prints one such line per merged tip (verify=DEFERRED) and a final one for the last tip carrying the single verify verdict (files = total, batch=<n>).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from collections import deque
from contextlib import nullcontext
import os
import re
import shutil
import subprocess
import sys
import time
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)
sys.path.insert(0, HERE)
import _git_util  # noqa: E402  (shared git helpers)
from lane_sim_rows import SIM_ROWS, default_sim_install, run_sim_rows, sim_rows_satisfied, sim_sensitive_paths, tip_changed_paths  # noqa: E402,F401

#: Commit trailer line(s): --trailer or $MC2_COMMIT_TRAILER (or $MC2_MERGE_TRAILER); none by default (the merging model names itself).
TRAILER = os.environ.get("MC2_COMMIT_TRAILER") or os.environ.get("MC2_MERGE_TRAILER", "")
FOLLOWUP_SUBJECT = "merge follow-up: ratchet baselines tightened at merged HEAD"
ARTIFACT_SUBJECT = "merge follow-up: regenerated artifacts"
DERIVED_SUBJECT = "merge follow-up: regenerated derived inventories (gate floor, log tag baseline) and tightened ratchet baselines"
LOG_TAG_BASELINE = "tests/unit/log_tag_baseline.json"
RETIRED_NAMES = "Gates/retired_names.json"

# Explicit output scopes: never stage unrelated work or compiler scratch files.
ARTIFACTS = [
    "tests/unit/log_tag_baseline.json", "tests/unit/pointer_order_baseline.json",
    "docs/render-backend-seams/gated-layout-macros.md",
    "docs/render-backend-seams/binding-slot-occupancy.json",
    "tools/shader_reflect/expected", "shaders/spv",
    "tests/unit/own_inventory_baseline.json", "tests/callers.json",
    ".claude/cleanup/vk-ownership-ledger.md",
]
GENERATORS = [
    ("log tags", "tools/log_tag_census.py", ["--write"]),
    ("pointer order", "tools/pointer_order_scan.py", ["--write-baseline"]),
    ("gated layout", "scripts/check-gated-layout-macros.py", ["--write", "--quiet"]),
    ("binding slots", "scripts/check-binding-slots.py", ["--json", ARTIFACTS[3], "--quiet"]),
    ("shader reflection", "tools/shader_reflect/reflect.py", ["--update"]),
    ("SPIR-V pilots", "tools/shader_offline_build/build_variants.py", []),
    ("ownership inventory", "tools/own_inventory.py", ["--write-baseline"]),
    ("script callers", "tools/callers.py", []),
    ("Vulkan ownership", "tools/vk_ownership_ledger.py", ["--write"]),
]

# Files that are pure functions of the tree: on conflict the lane's copy is taken and the
# ratchet --tighten run below regenerates them against the merged result.
REGENERABLE = {
    "tools/file_size_ratchet.baseline.json",
    "tools/cxx17_ratchet/baseline.json",
    "tools/axis_swizzle_ratchet.baseline.json",
    "scripts/gl_shader_variants.json",
}
REGENERABLE.update(p for p in ARTIFACTS if p.endswith((".json", ".md")))


# Generated files: their content is a pure function of the tree. A conflict here is
# resolved to ours and the matching regenerator rewrites them against the merged tree.
GENERATED = {
    "Gates/gate_index.gen.h", "Gates/gate_table.json", "Gates/feature_only_rows.json",
    "RenderCore/RendererFeatureRegistry.h", "docs/flags.auto.yaml",
    "tests/callers.json", "docs/split-map.md",
    "docs/background-launch-inventory.md", "docs/background-launch-child-baseline.json",
    "tests/unit/log_tag_baseline.json",
}
# Not generated wholesale: only its population floor is. Merged with the floor line neutralized.
TEST_GATE = "tests/unit/test_gate.cpp"
FLOOR_LINE = re.compile(r"^[ \t]*REQUIRE\(n == \d+\);.*$", re.MULTILINE)
GATE_GENERATED = {"Gates/gate_index.gen.h", "Gates/gate_table.json", "Gates/feature_only_rows.json",
                  "RenderCore/RendererFeatureRegistry.h", "docs/flags.auto.yaml"}


# (files, script relative to the root, args) run after a generated-file conflict was taken as ours.
GENERATED_REGENERATORS = (
    # --sync-verified rebuilds the compiler dump (a lane may add rows); --write only re-derives.
    (GATE_GENERATED, "tools/gate_gen.py", ["--sync-verified"]),
    ({"docs/split-map.md"}, "tools/split/split_map.py", ["--write"]),
    ({"docs/background-launch-inventory.md", "docs/background-launch-child-baseline.json"},
     "scripts/check-background-launch.py", ["--write", "--quiet"]),
    ({"tests/callers.json"}, "tools/callers.py", []),
    ({"tests/unit/log_tag_baseline.json"}, "tools/log_tag_census.py", ["--write"]),
)


def is_generated(path):
    return path in GENERATED


def test_gate_only_floor(root: str, sha: str, head_ref: str = "HEAD") -> bool:
    """True when the lane's test_gate.cpp differs from the merge base only on the population floor."""
    base = git_out(root, "merge-base", head_ref, sha).strip()
    def body(rev):
        r = git(root, "show", f"{rev}:{TEST_GATE}")
        return FLOOR_LINE.sub("", r.stdout) if r.returncode == 0 else None
    before, after = body(base), body(sha)
    return before is not None and before == after


def is_regenerable(path):
    return path in REGENERABLE or is_generated(path) or any(path.startswith(scope + "/") for scope in ARTIFACTS)
# The baselines the follow-up commit may contain (never a glob over tools/).
TIGHTENED_BASELINES = [
    "tools/file_size_ratchet.baseline.json",
    "tools/cxx17_ratchet/baseline.json",
    "tools/axis_swizzle_ratchet.baseline.json",
]
# (label, script relative to the tools dir, args)
RATCHETS = [
    ("file_size", "file_size_ratchet.py", ["--check", "--tighten", "--worktree"]),
    ("cxx17", "cxx17_ratchet/cxx17_ratchet.py", ["--check", "--tighten", "--worktree"]),
    ("axis_swizzle", "axis_swizzle_ratchet.py", ["--check", "--tighten", "--worktree"]),
    ("split_map", "split/split_map.py", ["--check"]),
]
# Row tables where both sides append rows: merged mechanically (keep both, the lane's new rows renumbered after ours).
BUG_TABLES = {".claude/cleanup/BUGS-FOUND-FIXED.md"}
ROW = re.compile(r"^\| (\d+) \|")
UNMERGED = {"DD", "AU", "UD", "UA", "DU", "AA", "UU"}


class Stop(Exception):
    def __init__(self, code: int, msg: str):
        super().__init__(msg)
        self.code = code


def run(root: str, cmd: list[str], env: dict | None = None) -> subprocess.CompletedProcess:  # non-git commands (the ratchets)
    return subprocess.run(cmd, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          encoding="utf-8", errors="replace", **console_child.child_kwargs(redirected=True))


def run_logged(root, cmd, env=None, log_path=None):
    """Stream merged child output to the console and optional persistent log; retain a short failure tail."""
    if log_path:
        os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
    with open(log_path, "a", encoding="utf-8") if log_path else nullcontext() as log:
        with subprocess.Popen(cmd, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding="utf-8", errors="replace",
                              **console_child.child_kwargs(redirected=True)) as child:
            tail = deque(maxlen=8)
            failures = []
            for line in child.stdout:
                print(line, end="", flush=True)
                if log:
                    log.write(line)
                    log.flush()
                tail.append(line)
                if failure_line(line):
                    failures.append(line)
            return subprocess.CompletedProcess(cmd, child.wait(), "".join(dict.fromkeys(failures + list(tail))))


def failure_line(line):
    """Keep pytest/doctest names and source locations even before a long tail."""
    return bool(re.search(r"FAILED|FAIL:|ERROR|VIOLATION|PERF_COUNTER_WARN|test case:|TEST CASE:|\.(?:py|cpp|h)[:(]\d+", line))


def failure_summary(output):
    rows = output.splitlines()
    return "\n".join(dict.fromkeys([line for line in rows if failure_line(line)] + rows[-8:]))


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


def plan(root: str, ref: str, head_ref: str = "HEAD") -> dict:
    """Facts that decide whether the merge may proceed (head_ref: the commit to merge into; a dry-run batch passes a virtual one).
    Raises Stop on a refusal."""
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
    head = git_out(root, "rev-parse", head_ref)
    contained = git(root, "merge-base", "--is-ancestor", sha, head).returncode == 0
    base = git_out(root, "merge-base", head, sha) if not contained else sha
    lane_files = lines(git_out(root, "diff", "--name-only", base, sha))
    nifty_files = lines(git_out(root, "diff", "--name-only", base, head))
    dirty = {p for _, p in entries}
    overlap = sorted(set(lane_files) & dirty)
    return {"sha": sha, "head": head, "base": base, "contained": contained, "lane_files": lane_files,
            "nifty_files": nifty_files, "overlap": overlap}


def predicted_conflicts(root: str, sha: str, head_ref: str = "HEAD") -> list[str] | None:
    """Conflicted paths a real merge would hit (None when git cannot predict)."""
    return virtual_merge(root, sha, head_ref)[1]


def virtual_merge(root: str, sha: str, head_ref: str = "HEAD") -> tuple[str | None, list[str] | None]:
    """(result tree, conflicted paths) of merging sha into head_ref without touching the worktree; (None, None) when git cannot say."""
    r = git(root, "merge-tree", "--write-tree", "--name-only", "--no-messages", head_ref, sha)
    if r.returncode in (0, 1):
        out = lines(r.stdout)
        return (out[0] if out else None), ([] if r.returncode == 0 else out[1:])  # first line is the result tree
    return None, None


def resolved_tree(root, tree, resolutions):
    """Give later batch tips the reviewed tree, including deletions, during planning."""
    if not tree or not resolutions:
        return tree
    with tempfile.TemporaryDirectory(prefix="lane-merge-index-") as temp:
        env = dict(os.environ, GIT_INDEX_FILE=os.path.join(temp, "index"))
        def indexed(*args, data=None):
            r = subprocess.run(["git", "-C", root, *args], input=data, env=env,
                               capture_output=True, **console_child.child_kwargs(redirected=True))
            if r.returncode:
                raise Stop(5, "cannot plan reviewed conflict tree: " + r.stderr.decode("utf-8", "replace"))
            return r.stdout.decode("utf-8").strip()
        indexed("read-tree", tree)
        for path, data in resolutions.items():
            if data is None:
                indexed("update-index", "--force-remove", "--", path)
            else:
                row = git_out(root, "ls-tree", tree, "--", path)
                mode = row.split()[0] if row else "100644"
                blob = indexed("hash-object", "-w", "--stdin", data=data)
                indexed("update-index", "--add", "--cacheinfo", mode, blob, path)
        return indexed("write-tree")


def virtual_head(root: str, head_ref: str, sha: str, tree: str | None) -> str | None:
    """An unreferenced commit standing for head_ref + sha merged, so a dry-run batch can plan the next tip against it."""
    if not tree:
        return None
    r = git(root, "commit-tree", tree, "-p", head_ref, "-p", sha, "-m", "dry-run batch")
    return r.stdout.strip() if r.returncode == 0 else None


def trailer_args() -> list[str]:
    return ["-m", TRAILER] if TRAILER else []


def render_message(template: str, subject: str, n: int) -> str:
    """A batch --message template: {subject} is the tip's subject, {n} its 1-based position."""
    return template.replace("{subject}", subject).replace("{n}", str(n))


#: Build-input classifier retained for callers. Verification also refreshes SHA for tooling-only merges.
COMPILED_INPUTS = re.compile(r"\.(cpp|h|hpp|inc|c|glsl|hglsl|frag|vert|comp|slang|rc|def|ini|in|manifest)$|CMakeLists|\.cmake$|(^|/)cmake/|(^|/)Gates/|(^|/)shaders/|(^|/)scripts/(gen|generate|configure)[^/]*\.py$")


def compiled_inputs_changed(root: str, pre: str, post: str = "HEAD") -> bool:
    r = git(root, "diff", "--name-only", pre, post)
    return r.returncode != 0 or any(COMPILED_INPUTS.search(p) for p in lines(r.stdout))


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


def load_resolutions(specs: list[str]) -> dict[str, bytes | None]:
    """Read explicit conflict results before starting a merge; '-' means a reviewed deletion."""
    results = {}
    for spec in specs:
        path, sep, source = spec.partition("=")
        if (not sep or not path or "\\" in path or ":" in path or path.startswith("/")
                or any(part in ("", ".", "..", ".git") for part in path.split("/")) or path in results):
            raise Stop(5, f"invalid or duplicate conflict resolution: {spec}")
        if source == "-":
            results[path] = None
        else:
            try:
                with open(source, "rb") as f:
                    results[path] = f.read()
            except OSError as e:
                raise Stop(5, f"cannot read conflict resolution {source}: {e}") from e
        if results[path] is not None and any(line.startswith((b"<<<<<<< ", b"=======", b">>>>>>> "))
                                             for line in results[path].splitlines()):
            raise Stop(5, f"conflict markers remain in resolution for {path}")
    return results


def gate_count(root: str) -> int | None:
    """Rows in the committed gate table, or None when this tree has none (merge-test fixtures)."""
    try:
        return int(json.loads((Path(root) / "Gates" / "gate_table.json").read_text(encoding="utf-8"))["count"])
    except (OSError, KeyError, TypeError, ValueError):
        return None


def sync_test_gate_floor(root: str) -> bool:
    """Set the test_gate population floor to the gate_gen count; the merge runs no C++ build."""
    count = gate_count(root)
    path = Path(root) / "tests" / "unit" / "test_gate.cpp"
    if count is None or not path.is_file():
        return False
    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    updated, n = re.subn(r"REQUIRE\(n == \d+\);", f"REQUIRE(n == {count});", text, count=1)
    if not n or updated == text:
        return False
    if crlf:
        updated = updated.replace("\n", "\r\n")
    path.write_bytes(updated.encode("utf-8"))
    return True


def regenerate_generated(root: str, conflicted: list[str], lane_files=()) -> None:
    """Rewrite generated files taken as ours from the merged tree; the merge commit carries the result."""
    touched = set(conflicted)
    stageable = set(GENERATED)
    for files, script, args in GENERATED_REGENERATORS:
        if not touched & files:
            continue
        if not os.path.isfile(os.path.join(root, script)):
            if os.path.exists(os.path.join(root, "CLAUDE.md")):
                abort_and_clean(root, lane_files)
                raise Stop(5, f"generated conflict needs regenerator {script}, which is missing; merge aborted")
            continue
        r = run(root, [sys.executable, "-B", os.path.join(root, script), *args])
        print(f"lane_merge: regenerate {script}: {'OK' if r.returncode == 0 else 'FAIL'}", flush=True)
        if r.returncode:
            abort_and_clean(root, lane_files)
            raise Stop(5, f"regenerating {script} after a generated conflict failed:\n{r.stdout[-2000:]}")
        if script == "tools/gate_gen.py":
            # Sync also updates verification metadata in the authored row sources.
            stageable.add("Gates/gate_table.cpp")
            stageable.update(p.relative_to(root).as_posix()
                             for p in (Path(root) / "Gates").glob("gate_rows_*.inc"))
    if TEST_GATE in touched:
        sync_test_gate_floor(root)
    # The generators also refresh sibling outputs (gate_index.gen.h <- flags.auto.yaml).
    # Only files that differ from HEAD are staged; the conflicted ones already are.
    changed = sorted(touched | {p for p in stageable
                                if (Path(root) / p).exists() and git(root, "diff", "--quiet", "--", p).returncode})
    a = git(root, "add", "--", *changed)
    if a.returncode:
        abort_and_clean(root, lane_files)
        raise Stop(5, "could not stage the regenerated files; merge aborted: " + a.stdout.strip())


def do_merge(root: str, sha: str, message: str, lane_files=(), lock_path: str | None = None, lock_wait_s: float = 600,
             sleep=time.sleep, clock=time.monotonic, resolutions=None) -> None:
    """Merge --no-ff; auto-resolve regenerable conflicts to the lane's side, abort on anything else.

    A failure that is not a conflict is cleaned up (see abort_and_clean). When git says a file could not be replaced and a tidy run holds
    the build tree, wait up to lock_wait_s for its lock to clear and retry once."""
    lock_path = lock_path or os.path.join(root, TIDY_LOCK)
    for attempt in (1, 2):
        r = git(root, "merge", "--no-ff", "-m", message, *trailer_args(), sha)
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
    resolutions = resolutions or {}
    if set(resolutions) - set(conflicted):
        abort_and_clean(root, lane_files)
        raise Stop(5, "resolution paths do not match actual conflicts; merge aborted")
    for path, data in resolutions.items():
        try:
            if data is None:
                result = git(root, "rm", "--", path)
            else:
                os.makedirs(os.path.dirname(os.path.join(root, path)), exist_ok=True)
                with open(os.path.join(root, path), "wb") as f:
                    f.write(data)
                result = git(root, "add", "-u", "--", path)
            if result.returncode:
                raise OSError(result.stdout.strip())
        except OSError as e:
            abort_and_clean(root, lane_files)
            raise Stop(5, f"cannot apply resolution for {path}; merge aborted: {e}") from e
    conflicted = [p for p in conflicted if p not in resolutions]
    tables = [p for p in conflicted if p in BUG_TABLES]
    conflicted = [p for p in conflicted if p not in BUG_TABLES]
    for p in tables:
        if not resolve_row_table(root, p):
            abort_and_clean(root, lane_files)
            raise Stop(5, f"{p}: both sides changed the same row or the text outside the rows; it cannot be merged mechanically "
                          "(keep both and renumber by hand); merge aborted")
    floor_only = TEST_GATE in conflicted and test_gate_only_floor(root, sha)
    foreign = [p for p in conflicted if not is_regenerable(p) and not (floor_only and p == TEST_GATE)]
    if foreign or not (conflicted or tables or resolutions):
        abort_and_clean(root, lane_files)
        what = foreign or ["(merge failed without conflicts)"]
        raise Stop(5, "conflicts outside the regenerable set; merge aborted:\n  " + "\n  ".join(what)
                   + ("" if foreign else "\n" + r.stdout.strip()))
    generated = [p for p in conflicted if is_generated(p) or (floor_only and p == TEST_GATE)]
    for p in conflicted:
        side = "--ours" if p in generated else "--theirs"
        c = git(root, "checkout", side, "--", p)
        a = git(root, "add", "-u", "--", p) if c.returncode == 0 else c
        if c.returncode != 0 or a.returncode != 0:
            abort_and_clean(root, lane_files)
            raise Stop(5, f"could not take {side} for {p}; merge aborted: {(c.stdout + a.stdout).strip()}")
    if generated:
        regenerate_generated(root, generated, lane_files)
    c = git(root, "commit", "-m", message, *trailer_args())
    if c.returncode != 0:
        abort_and_clean(root, lane_files)
        raise Stop(5, "merge commit failed; merge aborted: " + c.stdout.strip())


def merge_row_table(base: str, ours: str, theirs: str) -> str | None:
    """Keep both sides of a numbered markdown row table. None when it cannot be done mechanically
    (a row both sides changed, or text outside the rows that the lane changed)."""
    def split(text):
        rows, other = {}, []
        for ln in text.split("\n"):
            m = ROW.match(ln)
            if m:
                rows[int(m.group(1))] = ln
            else:
                other.append(ln)
        return rows, other
    (br, bo), (orows, oo), (tr, to) = split(base), split(ours), split(theirs)
    if oo != to and to != bo:
        return None  # the lane changed text outside the rows
    merged = dict(orows)
    new = []
    for n in sorted(tr):
        t, b, o = tr[n], br.get(n), orows.get(n)
        if o == t:
            continue
        if b is None:
            new.append(t)  # a row the lane added (its number may be taken on our side)
        elif o is None:
            return None
        elif o == b:
            merged[n] = t
        elif t != b:
            return None  # both sides changed the same row
    nxt = max(list(orows) + list(br) + [0]) + 1
    renumbered = {}
    for t in new:
        renumbered[nxt] = ROW.sub(f"| {nxt} |", t, count=1)
        nxt += 1
    merged.update(renumbered)
    out, emitted = [], False
    last_row = max((i for i, ln in enumerate(ours.split("\n")) if ROW.match(ln)), default=None)
    for i, ln in enumerate(ours.split("\n")):
        m = ROW.match(ln)
        out.append(merged[int(m.group(1))] if m else ln)
        if i == last_row:
            out.extend(renumbered[k] for k in sorted(renumbered))
    return "\n".join(out)


def resolve_row_table(root: str, path: str) -> bool:
    """Resolve a conflicted BUG_TABLES file in place and stage it; False when it needs a human."""
    stages = []
    for n in (1, 2, 3):
        r = git(root, "show", f":{n}:{path}")
        if r.returncode != 0:
            return False
        stages.append(r.stdout)
    text = merge_row_table(*stages)
    if text is None:
        return False
    with open(os.path.join(root, path), "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return git(root, "add", "-u", "--", path).returncode == 0


def run_ratchets(root: str, tools_dir: str, *, tighten=True) -> list[tuple[str, int, str]]:
    results = []
    for label, rel, args in RATCHETS:
        script = os.path.join(tools_dir, *rel.split("/"))
        selected = args if tighten else [a for a in args if a != '--tighten']
        r = run(root, [sys.executable, "-B", script, *selected])
        # every ratchet always runs; a failing one reports its whole violation list, not just the last lines (W78: first hit only)
        tail = "\n".join(lines(r.stdout)[-60:] if r.returncode else lines(r.stdout)[-3:])
        results.append((label, r.returncode, tail))
    return results


def git_bash(which=shutil.which, exists=os.path.exists, environ=None) -> str:
    """The bash that runs the verify .sh scripts. WHY: CreateProcess searches System32 before PATH, so a bare
    "bash" from a Python child is the WSL launcher (fails with an HCS error on a host without a distro); the
    Git for Windows bash next to the git on PATH is the one the scripts are written for. MC2_BASH overrides."""
    env = os.environ if environ is None else environ
    if env.get("MC2_BASH"):
        return env["MC2_BASH"]
    git = which("git")
    if git:
        gitroot = os.path.dirname(os.path.dirname(os.path.normpath(git)))   # <Git>/cmd/git.exe or <Git>/bin/git.exe
        for cand in (os.path.join(gitroot, "usr", "bin", "bash.exe"), os.path.join(gitroot, "bin", "bash.exe")):
            if exists(cand):
                return cand
    found = which("bash")
    if found and "system32" not in found.lower() and "windowsapps" not in found.lower():
        return found
    return "bash"


VERIFY_STEPS = {
    "build": [("build", ["scripts/ninja_build.py"]), ("doctest", ["scripts/run-unit-tests.sh"])],
    "full": [("build", ["scripts/ninja_build.py"]), ("doctest", ["scripts/run-unit-tests.sh"]), ("board", ["scripts/check-contracts.sh"])],
}


#: Seconds-cheap contract rows a lane must see before the window (W109 found them late): (label, script relative to the root, args).
EARLY_ROWS = [("sim_include_firewall", "scripts/check_sim_include_firewall.py", []),
              ("log_tag_census", "tools/log_tag_census.py", ["--check"])]


#: Seconds-cheap ownership/callers checks, run before the ~18 min build + doctest so a duplicate-ownership red shows at once.
QUICK_ROWS = [("script_callers", ["tools/callers.py", "--check"]),
              ("test_ownership", ["-m", "pytest", "scripts/tests/test_contract_coverage.py", "-q", "-p", "no:cacheprovider"])]


def quick_rows(root: str, runner=run) -> list[tuple[str, int, str]]:
    """Run QUICK_ROWS (cwd = root) on the merged tree. -> the failing [(label, rc, tail)] (empty = green)."""
    failed = []
    for label, args in QUICK_ROWS:
        missing = [a for a in args if a.endswith(".py") and not os.path.exists(os.path.join(root, a))]
        if missing:
            if os.path.abspath(root) == os.path.abspath(os.path.join(HERE, "..")):   # the real repo: a renamed script must not skip silently
                failed.append((label, -1, "quick row script missing: " + ", ".join(missing)))
            continue  # a throwaway root without the script has nothing to check
        r = runner(root, [sys.executable] + args)
        if r.returncode != 0:
            if label == "script_callers":
                for path in re.findall(r"tracked script has no caller and is not allowlisted: ([^\r\n]+)", r.stdout):
                    reason = ("Pytest-discovered module through the scripts_tests/tools_tests contract row."
                              if Path(path).name.startswith("test_") or Path(path).name.endswith("_test.py")
                              else "Operator-run entry point; document its intended invocation here.")
                    print("lane_merge: review the caller wiring; if intentional, add to tests/callers_allowlist.json: "
                          + json.dumps(path) + ": " + json.dumps(reason), file=sys.stderr)
            failed.append((label, r.returncode, failure_summary(r.stdout)))
    return failed


def host_jobs():
    try:
        return str(max(1, int(Path("A:/Games/mc2-ops/JOBS").read_text().strip())))
    except (OSError, ValueError):
        return str(min(6, os.cpu_count() or 2))


def verify_env(root: str) -> dict:
    """Leave the canonical Ninja tree ready for deploy_payload.py --ninja.

    The orchestrator owns this worktree exclusively during a merge window.
    """
    return dict(os.environ, MC2_BUILD_JOBS=host_jobs(), CONTRACTS_JOBS=host_jobs(), MC2_NINJA_TREE="build64-ninja", MC2_UNIT_BUILD_DIR=os.path.join(root, "build-cc-tests"))


def verify_merged(root: str, mode: str, runner=None) -> tuple[str, list[tuple[str, int, str]]]:
    """Build and test the merged tree. -> ("OK"|"FAIL"|"SKIPPED", [(step, rc, tail)]). Stops at the first failing step."""
    if mode == "none":
        return "SKIPPED", []
    results = []
    for label, (script, *rest) in VERIFY_STEPS[mode]:
        path = os.path.join(root, script)
        if not os.path.exists(path):
            return ("SKIPPED" if not results else "OK"), results
        cmd = [git_bash(), path] + rest if script.endswith(".sh") else [sys.executable, path] + rest
        log_path = os.path.join(root, "build64-ninja", "lane-merge-verify-" + label + ".log")
        print(f"lane_merge: {label} log: {log_path}", flush=True)
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "w", encoding="utf-8") as log:
            log.write(subprocess.list2cmdline(cmd) + "\n")
        if runner is None:
            r = run_logged(root, cmd, env=verify_env(root), log_path=log_path)
        else:
            r = runner(root, cmd, env=verify_env(root))
            with open(log_path, "a", encoding="utf-8") as log:
                log.write(r.stdout)
            print(r.stdout, end="", flush=True)
        tail = failure_summary(r.stdout)
        results.append((label, r.returncode, tail))
        if r.returncode != 0:
            return "FAIL", results
    return "OK", results


def exe_matches_head(root: str, tree: str = "build64-ninja") -> bool | None:
    """True/False: does the built mc2.exe embed the worktree HEAD? None when there is no exe or no readable fingerprint."""
    exe = os.path.join(root, tree, "mc2.exe")
    if not os.path.isfile(exe):
        return None
    sys.path.insert(0, os.path.join(root, "scripts"))
    try:
        from exe_fingerprint import extract, sha_agrees, build_current
        fp = extract(exe)
        if not fp.get("present"):
            return None
        return bool(sha_agrees(fp.get("sha"), head := git_out(root, "rev-parse", "HEAD")) or build_current(fp.get("sha"), head, root, bool(fp.get("dirty"))))  # current ancestor exe: no relink for data-only commits
    except Exception:  # noqa: BLE001 - an unreadable fingerprint must not fail the window
        return None


def rebuild_if_head_moved(root: str, check=None, runner=None) -> tuple[bool, str]:
    """After repairs/re-freezes HEAD moved but a json-only commit does not relink the exe: rebuild before the gate.
    -> (ok, message). Not ok = the exe still fingerprints a different sha after the rebuild (the gate would refuse it)."""
    check = check or exe_matches_head
    if check(root) is not False:
        return True, ""
    print("lane_merge: exe fingerprint differs from HEAD; rebuilding before the gate", flush=True)
    r = (runner or run)(root, [sys.executable, os.path.join(root, "scripts", "ninja_build.py")], env=verify_env(root))
    if r.returncode:
        return False, "rebuild before the gate failed:\n" + failure_summary(r.stdout)
    if check(root) is False:
        return False, "exe fingerprint still differs from HEAD after a rebuild; the gate would refuse this exe"
    return True, ""


def log_tag_loosened(old: dict, new: dict) -> list[str]:
    """Why `new` is not a pure extension of the frozen census `old` (empty = only additions, safe to accept).

    An emitter that vanished, lost count, or changed keys/version/stream is a harness-visible regression a re-freeze would hide."""
    def table(doc):
        return {(r["tag"], r["ver"], r["stream"], tuple(r["keys"])): r["count"] for r in doc.get("emitters", [])}
    a, b = table(old), table(new)
    why = [f"emitter {k[0]} v{k[1]} stream={k[2]} keys={','.join(k[3])}: count {n} -> {b.get(k, 0)}"
           for k, n in sorted(a.items()) if b.get(k, 0) < n]
    for key in ("consumed_tags", "plain_tags"):
        why += [f"{key}: {t} no longer listed" for t in sorted(set(old.get(key, [])) - set(new.get(key, [])))]
    return why


def gate_floor_loosened(old_names: set[str], new_names: set[str], retired: set[str]) -> list[str]:
    """Gates that left the table without being listed as retired: a lower floor would hide losing them."""
    return sorted(old_names - new_names - retired)


def _git_json(root: str, rev: str, path: str):
    r = git(root, "show", f"{rev}:{path}")
    if r.returncode:
        return None
    try:
        return json.loads(r.stdout)
    except ValueError:
        return None


def gate_names(path: Path):
    try:
        return {g["name"] for g in json.loads(path.read_text(encoding="utf-8"))["gates"]}
    except (OSError, KeyError, TypeError, ValueError):
        return None


def regenerate_derived(root: str, base: str) -> list[str]:
    """Batch-end regeneration of the derived inventories that used to need a hand follow-up.

    Tighten-only: the log tag census may only gain emitters and the gate population floor may only follow additions
    (or retirements listed in Gates/retired_names.json). Anything else restores the file and stops the window (exit 6)."""
    changed = list(regenerate_all(root))
    live = Path(root) / LOG_TAG_BASELINE
    old = _git_json(root, "HEAD", LOG_TAG_BASELINE)
    if old is not None and live.is_file():
        bad = log_tag_loosened(old, json.loads(live.read_text(encoding="utf-8")))
        if bad:
            git(root, "checkout", "--", LOG_TAG_BASELINE)
            raise Stop(6, "log tag census loosened by the merge (not auto-accepted; fix the lane or re-freeze by hand):\n  "
                       + "\n  ".join(bad))
    names_now, table_old = gate_names(Path(root) / "Gates" / "gate_table.json"), _git_json(root, base, "Gates/gate_table.json")
    if names_now is not None and table_old is not None:
        retired = _git_json(root, "HEAD", RETIRED_NAMES) or []
        if isinstance(retired, dict):
            retired = retired.get("names") or list(retired)
        lost = gate_floor_loosened({g["name"] for g in table_old["gates"]}, names_now, set(retired))
        if lost:
            raise Stop(6, "gate table lost gates that are not in " + RETIRED_NAMES + " (population floor not lowered):\n  "
                       + "\n  ".join(lost))
        if sync_test_gate_floor(root):
            changed.append(TEST_GATE)
    return sorted(set(changed))


def commit_tightened(root: str, pre_dirty: set[str], artifacts=()) -> str | None:
    """Commit exactly the baselines the ratchets tightened; skip any that was already dirty."""
    now_dirty = {p for _, p in status_entries(root)}
    changed = sorted({p for p in (*TIGHTENED_BASELINES, *artifacts) if p in now_dirty and p not in pre_dirty})
    skipped = [p for p in TIGHTENED_BASELINES if p in now_dirty and p in pre_dirty]
    if skipped:
        print("lane_merge: baseline already had uncommitted edits, left alone: " + ", ".join(skipped),
              file=sys.stderr)
    if not changed:
        return None
    try:
        derived = {TEST_GATE, LOG_TAG_BASELINE} & set(changed)
        return _git_util.commit_paths(root, changed, DERIVED_SUBJECT if derived else FOLLOWUP_SUBJECT, TRAILER or None)
    except _git_util.GitError as e:
        raise Stop(5, "follow-up commit failed: " + str(e))


def artifact_changes(root):
    changed = set()
    for args in (("diff", "HEAD", "--name-only"), ("ls-files", "--others", "--exclude-standard")):
        result = _git_util.git(root, *args, "-z", "--", *ARTIFACTS)
        if result.returncode:
            raise Stop(6, "cannot list artifacts: " + result.stderr)
        changed.update(p for p in result.stdout.split("\0") if p)
    return sorted(changed)


def require_clean_generator_inputs(root):
    """Artifacts must describe committed HEAD, including inputs outside ARTIFACTS."""
    # Throwaway merge-test repos without generators do not regenerate anything.
    if os.path.exists(os.path.join(root, GENERATORS[0][1])):
        dirty = status_entries(root)
        if dirty:
            raise Stop(6, "regenerate-all requires a clean tree; commit or remove generator input edits first:\n  "
                       + "\n  ".join(path for _, path in dirty))


def regenerate_all(root, *, dry_run=False):
    """Regenerate the committed tree in isolation for dry runs; never bless preexisting edits."""
    if dry_run:
        # A disposable local clone lets every generator use its normal paths and
        # git inventory. No generator writes into the caller's checkout.
        proof_root = "E:/mc2-archive-2026-10/proofs" if os.path.isdir("E:/mc2-archive-2026-10/proofs") else None
        with tempfile.TemporaryDirectory(prefix="regen-", dir=proof_root) as scratch:
            snapshot = os.path.join(scratch, "tree")
            r = run(root, ["git", "clone", "--quiet", "--shared", "--no-checkout", os.path.abspath(root), snapshot])
            if r.returncode:
                raise Stop(6, r.stdout)
            sha = git_out(root, "rev-parse", "HEAD")
            git_out(snapshot, "checkout", "--quiet", "--detach", sha)
            changed = regenerate_all(snapshot)
            print(f"regenerate-all dry-run at {sha[:9]}: " + (", ".join(changed) or "nothing to change"))
            return changed
    overlaps = artifact_changes(root)
    if overlaps:
        raise Stop(6, "regenerate-all refuses preexisting artifact edits: " + ", ".join(sorted(overlaps)))
    require_clean_generator_inputs(root)
    # Tiny fixture repos used by merge tests do not carry the engine generators.
    if not os.path.exists(os.path.join(root, GENERATORS[0][1])):
        if os.path.exists(os.path.join(root, "CLAUDE.md")):
            raise Stop(6, "regenerate-all: missing log_tag_census.py")
        return []
    for label, script, args in GENERATORS:
        r = run(root, [sys.executable, "-B", os.path.join(root, script), *args])
        print(f"regenerate-all: {label}: {'OK' if r.returncode == 0 else 'FAIL'}", flush=True)
        if r.returncode:
            raise Stop(6, f"regenerate-all {label} failed:\n{r.stdout}")
    # Windows text-mode generators can otherwise dirty every expected JSON
    # with CRLF alone. Never decode/rewrite binary artifacts (notably SPIR-V).
    paths = git_out(root, "ls-files", "-z", "--", *ARTIFACTS).split("\0")
    for path in set(paths) | set(artifact_changes(root)):
        output = Path(root) / path
        if output.is_file() and output.suffix in {".json", ".md"}:
            data = output.read_bytes()
            lf = data.replace(b"\r\n", b"\n")
            if lf != data:
                output.write_bytes(lf)
    changed = artifact_changes(root)
    print("regenerate-all: " + (", ".join(changed) or "nothing to change"))
    if changed:
        print(git_out(root, "diff", "--stat", "--", *changed))
    return changed


PYTHON_SUITES = [
    ("scripts_tests", ["scripts/run_scripts_tests.py"]),
    ("tools_tests", ["scripts/run_tools_tests.py"]),
]


PRECHECK_TREE = os.environ.get("MC2_PRECHECK_TREE", "E:/mc2-installs/_precheck/tree")


def _inside(path: str, root: str) -> bool:
    path, root = os.path.abspath(path), os.path.abspath(root)
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:   # different drives
        return False


def precheck_scratch_base(root: str) -> str | None:
    """Scratch parent for the fast path: G:/mc2-tmp (lane-rules), else the OS temp dir. Never inside the main worktree."""
    base = "G:/mc2-tmp" if os.path.isdir("G:/mc2-tmp") else None
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
        self.fh.close()   # closing drops the OS lock


def ensure_precheck_tree(root: str, tree: str, commit: str) -> None:
    """Create the persistent warm worktree once (detached, outside the main worktree), then park it on `commit`."""
    if _inside(tree, root):
        raise Stop(5, f"warm precheck tree {tree} is inside {root}")
    if not os.path.exists(os.path.join(tree, ".git")):
        os.makedirs(os.path.dirname(os.path.abspath(tree)), exist_ok=True)
        r = git(root, "worktree", "add", "--detach", "--force", os.path.abspath(tree), commit)
        if r.returncode:
            raise Stop(5, "cannot create warm tree: " + failure_summary(r.stdout + r.stderr))
        from bootstrap_worktree_build import ensure_deps
        ensure_deps(os.path.abspath(tree), os.path.abspath(root))
    else:
        r = git(tree, "checkout", "--quiet", "-f", "--detach", commit)
        if r.returncode:
            raise Stop(5, "warm tree checkout failed: " + failure_summary(r.stdout + r.stderr))


def stale_inventories(root: str, commit: str, regen=None) -> list[str]:
    """A9: derived inventories that regenerating at `commit` would change (empty = fresh). The batch regenerates and
    commits them anyway; failing here moves that cost from the merge window to the lane."""
    regen = regen or regenerate_all
    proof_root = "E:/mc2-archive-2026-10/proofs" if os.path.isdir("E:/mc2-archive-2026-10/proofs") else None
    with tempfile.TemporaryDirectory(prefix="stale-inv-", dir=proof_root) as scratch:
        snapshot = os.path.join(scratch, "tree")
        r = run(root, ["git", "clone", "--quiet", "--shared", "--no-checkout", os.path.abspath(root), snapshot])
        if r.returncode:
            raise Stop(5, "clone failed: " + failure_summary(r.stdout))
        git_out(snapshot, "checkout", "--quiet", "--detach", commit)
        return list(regen(snapshot))


def lint_rows(py: list[str], base_ref: str, ref: str) -> list[tuple[str, list[str], str]]:
    """The seconds-cheap checks a precheck would fail on: (label, command, fix hint). Same tables as precheck."""
    rows = [(label, py + [os.path.join("tools", script), *[a for a in args if a != "--tighten"]],
             "fix the code, or lower the baseline via the ratchet's own --tighten (never raise it)") for label, script, args in RATCHETS]
    rows += [("callers-refresh", py + ["tools/callers.py"], ""),
             ("callers", py + ["tools/callers.py", "--check"], "wire the script into a caller or add it to tests/callers_allowlist.json with a reason")]
    rows += [(label, py + [script, *args], "see the row's own output") for label, script, args in EARLY_ROWS]
    rows.append(("ungated_tags", py + ["tools/ungated_log_tags.py", "--base", base_ref, "--head", ref],
                 "gate the new log tag or register it as the check says"))
    return rows


def lint(root: str, ref: str, runner=None, regen=None) -> int:
    """`--lint REF`: the precheck's fast local checks (ratchets, callers, early rows, ungated tags, stale inventories)
    run concurrently at REF in shared clones; every red is printed with its fix. No Python suites, no link. Exit 0 = clean."""
    runner = runner or run
    started = time.monotonic()
    failed: list[str] = []
    try:
        sha = git_out(root, "rev-parse", ref + "^{commit}")
        base_ref = git_out(root, "rev-parse", "claude/nifty-mendeleev^{commit}")
    except Stop as exc:
        print("LINT FAIL " + str(exc))
        return 1
    rows = lint_rows([sys.executable, "-B"], base_ref, sha)
    # callers-refresh must precede callers in one tree; every other row gets its own tree so they run concurrently.
    chains = [[r for r in rows if r[0].startswith("callers")]] + [[r] for r in rows if not r[0].startswith("callers")]
    with tempfile.TemporaryDirectory(prefix="lane-lint-", dir=precheck_scratch_base(root)) as scratch:
        snap = os.path.join(scratch, "tree")   # one checkout shared by all rows: only callers-refresh writes (tests/callers.json), which no other row reads
        r = run(root, ["git", "clone", "--quiet", "--shared", "--no-checkout", os.path.abspath(root), snap])
        if r.returncode:
            print("LINT FAIL clone failed: " + failure_summary(r.stdout))
            return 1
        git_out(snap, "checkout", "--quiet", "--detach", sha)

        def chain(i):
            out = []
            for label, cmd, hint in chains[i]:
                t0 = time.monotonic()
                r = runner(snap, cmd)
                out.append((label, r.returncode, time.monotonic() - t0, failure_summary(r.stdout), hint))
            return out
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=len(chains) + 1) as pool:
            inv = pool.submit(lambda: list(regen(root, sha)) if regen else stale_inventories(root, sha))   # the slowest check: overlap it with the rows
            for f in [pool.submit(chain, i) for i in range(len(chains))]:
                try:
                    results = f.result()
                except (Stop, OSError) as exc:
                    failed.append("lint:" + str(exc).replace("\n", " "))
                    continue
                for label, rc, secs, tail, hint in results:
                    if label == "callers-refresh":
                        continue
                    print(f"lint: {label}: {'PASS' if rc == 0 else 'FAIL'} ({secs:.0f}s)", flush=True)
                    if rc:
                        failed.append(label)
                        print("  " + tail.replace("\n", "\n  "), flush=True)
                        if hint:
                            print("  fix: " + hint, flush=True)
    try:
        changed = inv.result()
    except Stop as exc:
        changed = []
        failed.append("inventories:" + str(exc).replace(chr(10), " "))
    print("lint: inventories: " + ("FAIL (stale: " + ", ".join(changed) + "; fix: tools/lane_merge.py --regenerate-all and commit)" if changed else "PASS"), flush=True)
    if changed:
        failed.append("inventories-stale")
    print("LINT " + ("FAIL " + ", ".join(failed) if failed else "PASS") + f" WALL={time.monotonic() - started:.0f}s (the precheck still runs the Python suites and the link)")
    return int(bool(failed))


PRECHECK_RECEIPTS = os.environ.get("MC2_PRECHECK_RECEIPTS", "A:/Games/mc2-ops/precheck")


def receipt_path(sha: str) -> str:
    if not re.fullmatch(r"[0-9a-f]{40}", sha or ""):   # a mocked or truncated rev-parse must never forge a receipt file
        raise ValueError(f"precheck receipt needs a full 40-hex commit SHA, got {sha!r}")
    return os.path.join(PRECHECK_RECEIPTS, sha + ".json")


def write_receipt(root: str, tip: str, with_link, verdict: str, sim_rows: dict | None = None) -> None:
    """A green `--precheck` leaves a receipt for that exact tip SHA (`--batch` refuses one without); sim_rows = {required, override, paths, install, rows}."""
    sha = git_out(root, "rev-parse", tip + "^{commit}")
    os.makedirs(PRECHECK_RECEIPTS, exist_ok=True)
    body = {"tip": sha, "base_head": git_out(root, "rev-parse", "HEAD"), "base_tree": git_out(root, "rev-parse", "HEAD^{tree}"),
            "verdict": verdict, "link": bool(with_link), "when": time.time()}
    if sim_rows is not None:
        body["sim_rows"] = sim_rows
    tmp = receipt_path(sha) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(body, f)
    os.replace(tmp, receipt_path(sha))


def missing_receipts(root: str, refs: list[str]) -> list[str]:
    out = []
    for ref in refs:
        sha = git_out(root, "rev-parse", ref + "^{commit}")
        try:
            with open(receipt_path(sha), encoding="utf-8") as f:
                receipt = json.load(f)
            ok = receipt.get("verdict") == "PASS" and sim_rows_satisfied(root, sha, receipt)
        except (OSError, ValueError):
            ok = False
        if not ok:
            out.append(sha)
    return out


def precheck(root: str, tip: str, with_link: bool | None = None, sim_rows: bool = True, sim_install: str | None = None,
             sim_runner=None, sim_quiet=None) -> int:
    """Grade the virtual nifty merge, including Python suites, without merging it.

    Checks (minutes): conflict check, 5 ratchets, callers refresh+check and Python suites in disposable shared
    clones outside the main worktree. --with-link also runs, concurrently, an incremental configure + link of mc2, mc2fx,
    mc2fx_preview in the persistent warm tree (PRECHECK_TREE); only that build holds its exclusive lock.
    with_link=None (default) turns the link on when the tip changes compiled inputs (compiled_inputs_changed); False (--no-link) opts out."""
    failed = []
    timings = []
    started = time.monotonic()

    def timed(label, fn):
        t0 = time.monotonic()
        try:
            return fn()
        finally:
            timings.append((label, time.monotonic() - t0))

    def stage(label, workdir, command, ok=(0,)):
        t0 = time.monotonic()
        env = verify_env(workdir)
        if label in {p[0] for p in PYTHON_SUITES}:
            env["MC2_IN_GATE"] = "1"   # the gate's env: a test that breaks under it must fail here, not in the window (W112)
        r = run(workdir, command, env=env)
        secs = time.monotonic() - t0
        timings.append((label, secs))
        word = "KNOWN" if r.returncode == 3 and 3 in ok else "PASS" if r.returncode == 0 else "FAIL"
        print(f"precheck: {label}: {word} ({secs:.0f}s)", flush=True)
        if r.returncode not in ok:
            failed.append(label + ":" + failure_summary(r.stdout).replace("\n", " "))

    try:
        def virtual():
            base = git_out(root, "rev-parse", "claude/nifty-mendeleev^{commit}")
            sha = git_out(root, "rev-parse", tip + "^{commit}")
            tree, conflicts = virtual_merge(root, sha, base)
            if tree is None or conflicts is None:
                raise Stop(5, "merge-tree unavailable")
            if conflicts:
                raise Stop(5, "conflicts=" + ",".join(conflicts))
            commit = sha if base == sha else virtual_head(root, base, sha, tree)
            if not commit:
                raise Stop(5, "cannot create virtual commit")
            return commit
        commit = timed("virtual-merge", virtual)
        base_ref = git_out(root, "rev-parse", "claude/nifty-mendeleev^{commit}")
        if with_link is None:
            with_link = compiled_inputs_changed(root, base_ref, commit)
            print(f"precheck: link {'ON' if with_link else 'off'} (compiled inputs {'changed' if with_link else 'unchanged'}; --no-link/--with-link override)", flush=True)
        py = [sys.executable, "-B"]
        steps = [(label, py + [os.path.join("tools", script), *[a for a in args if a != "--tighten"]])
                 for label, script, args in RATCHETS]
        # A virtual merge can shift caller locations without conflicting in JSON.
        # Refresh that generated input, then retain the strict wiring check.
        steps += [("callers-refresh", py + ["tools/callers.py"]),
                  ("callers", py + ["tools/callers.py", "--check"])]
        steps += [(label, py + [script, *args]) for label, script, args in EARLY_ROWS]
        steps += [(label, py + args) for label, args in PYTHON_SUITES]

        tag_step = ("ungated_tags", py + [os.path.join(root, "tools", "ungated_log_tags.py"), "--base", base_ref, "--head", commit])
        stage(tag_step[0], root, tag_step[1])

        def inventory_fresh():
            changed = stale_inventories(root, commit)
            word = "FAIL" if changed else "PASS"
            print(f"precheck: inventories: {word}" + (" (stale: " + ", ".join(changed) + "; regenerate with tools/lane_merge.py --regenerate-all and commit)" if changed else ""), flush=True)
            if changed:
                failed.append("inventories-stale:" + ",".join(changed))
        timed("inventories", inventory_fresh)

        def build_job():
            # The warm tree lock covers only checkout + configure + link; ratchets, callers and pytest run in shared
            # clones, so a queued precheck waits for a build (minutes), never for another precheck's whole run (W113: 1629 s).
            with TreeLock(PRECHECK_TREE + ".lock"):
                timed("tree-checkout", lambda: ensure_precheck_tree(root, PRECHECK_TREE, commit))
                stage("configure", PRECHECK_TREE, py + ["scripts/configure_ninja.py", "--build-dir", "build64-ninja"])
                stage("link-mc2", PRECHECK_TREE, py + ["scripts/ninja_build.py", "mc2", "-j" + host_jobs()])
                stage("link-tools", PRECHECK_TREE, py + ["scripts/ninja_build.py", "mc2fx", "mc2fx_preview", "-j" + host_jobs()])

        # Independent chains run concurrently, each in its own shared clone (no shared generated files).
        chains = [[s for s in steps if s[0] in {r[0] for r in RATCHETS}],
                  [s for s in steps if s[0].startswith("callers") or s[0] in {r[0] for r in EARLY_ROWS}]] + [[s] for s in steps if s[0] in {p[0] for p in PYTHON_SUITES}]
        with tempfile.TemporaryDirectory(prefix="lane-precheck-", dir=precheck_scratch_base(root)) as scratch:
            def chain(i):
                snapshot = os.path.join(scratch, f"tree{i}")
                t0 = time.monotonic()
                r = run(root, ["git", "clone", "--quiet", "--shared", "--no-checkout", os.path.abspath(root), snapshot])
                if r.returncode:
                    raise Stop(5, "clone failed: " + failure_summary(r.stdout))
                git_out(snapshot, "checkout", "--quiet", "--detach", commit)
                timings.append((f"clone{i}", time.monotonic() - t0))
                for label, command in chains[i]:
                    stage(label, snapshot, command, (0, 3) if label == "tools_tests" else (0,))
            from concurrent.futures import ThreadPoolExecutor
            jobs = [lambda i=i: chain(i) for i in range(len(chains))] + ([build_job] if with_link else [])
            with ThreadPoolExecutor(max_workers=len(jobs)) as pool:
                for f in [pool.submit(job) for job in jobs]:
                    try:
                        f.result()
                    except (Stop, OSError) as exc:
                        failed.append(str(exc).replace("\n", " "))
    except (Stop, OSError) as exc:
        failed.append(str(exc).replace("\n", " "))
    sim = {"required": False, "override": not sim_rows, "paths": [], "install": None, "rows": {}}
    try:
        sim["paths"] = sim_sensitive_paths(tip_changed_paths(root, tip))
        sim["required"] = bool(sim["paths"])
    except (Stop, OSError, KeyError, ValueError, RuntimeError) as exc:
        failed.append("sim-rows: cannot classify the tip's paths: " + str(exc).replace("\n", " "))
    if sim["required"] and not sim_rows:
        print(f"precheck: sim rows SKIPPED by --no-sim-rows ({len(sim['paths'])} sim/save-sensitive path(s); recorded in the receipt)", flush=True)
    elif sim["required"] and not failed:   # a red earlier stage already fails the precheck; spare the game runs
        sim["install"] = sim_install or default_sim_install(root)
        print(f"precheck: sim/save-sensitive paths ({', '.join(sim['paths'][:3])}...): running {', '.join(SIM_ROWS)} on {sim['install']}", flush=True)
        sim["rows"] = timed("sim-rows", lambda: run_sim_rows(root, git_out(root, "rev-parse", tip + "^{commit}"), sim["install"], runner=sim_runner, quiet=sim_quiet))
        failed += [f"sim-row {row}: {v}" for row, v in sim["rows"].items() if v != "PASS"]
        print("\n".join(f"precheck: {row}: {v}" for row, v in sim["rows"].items()), flush=True)
    print("precheck timing (s): " + "  ".join(f"{label}={secs:.0f}" for label, secs in timings)
          + f"  WALL={time.monotonic() - started:.0f}", flush=True)
    link = "link=mc2,mc2fx,mc2fx_preview" if with_link else "link=skipped(batch --verify full and the gate build)"
    print("PRECHECK " + ("FAIL " + "; ".join(failed) if failed else f"PASS conflicts=0 ratchets=5 {link} callers=OK python=OK"))
    if not failed:
        try:
            write_receipt(root, tip, with_link, "PASS", sim)
        except (OSError, Stop) as exc:
            print(f"precheck: receipt not written ({exc}); --batch will refuse this tip")
    return int(bool(failed))


def pre_gate(root: str, exe: str | None = None) -> int:
    """Everything that used to fail only after the gate started (W79: three reruns), run in order at the merged HEAD.
    Every step runs; one verdict line at the end. Exit 0 = clean."""
    py = [sys.executable, "-B"]
    steps = [
        ("regenerate-all", None),
        ("background-launch", py + [os.path.join(root, "scripts", "check-background-launch.py"), "--quiet"]),
        ("gate_gen", py + [os.path.join(root, "tools", "gate_gen.py"), "--check"]),
        ("cxx17", py + [os.path.join(root, "tools", "cxx17_ratchet", "cxx17_ratchet.py"), "--check", "--worktree"]),
    ]
    steps += [(label, py + [os.path.join(root, script), *args]) for label, script, args in EARLY_ROWS]
    exe = exe or os.path.join(root, "build64-ninja", "mc2.exe")
    if os.path.isfile(exe):   # parse-level sanity of the headless smoke path; --resolve-only launches nothing
        steps.append(("headless-smoke", py + [os.path.join(root, "scripts", "run_smoke.py"), "--headless", "--ticks", "10",
                                              "--resolve-only", "--exe", exe]))
    failed = []
    if os.path.isfile(exe) and exe_matches_head(root, os.path.relpath(os.path.dirname(exe), root)) is False:
        print("pre-gate: exe-fingerprint: FAIL\nthe built exe embeds a different sha than HEAD; rebuild (scripts/ninja_build.py) before the gate", flush=True)
        failed.append("exe-fingerprint")
    for label, cmd in steps:
        if cmd is None:
            try:
                changed = regenerate_all(root, dry_run=True)
                rc, out = (1 if changed else 0), ("would change: " + ", ".join(changed))
            except Stop as exc:
                rc, out = 1, str(exc)
        else:
            r = run(root, cmd)
            rc, out = r.returncode, "\n".join(lines(r.stdout)[-15:])
        print(f"pre-gate: {label}: {'OK' if rc == 0 else 'FAIL'}", flush=True)
        if rc:
            print(out, flush=True)
            failed.append(label)
    print("PRE-GATE " + ("PASS" if not failed else "FAIL: " + ", ".join(failed))
          + ("" if os.path.isfile(exe) else " (headless-smoke skipped: no exe)"))
    return 1 if failed else 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", nargs="?", help="lane tip: sha or branch")
    ap.add_argument("--lint", metavar="REF", help="seconds-fast subset of --precheck (ratchets, callers, early rows, ungated tags, stale inventories) with a fix per red; run it before --precheck")
    ap.add_argument("--precheck", metavar="TIP", help="offline virtual nifty merge: conflicts, ratchets, callers, Python suites (fast, no native build; --with-link adds links)")
    ap.add_argument("--with-link", action="store_true",
                    help="with --precheck: also link mc2, mc2fx, mc2fx_preview incrementally in the warm tree "
                         "($MC2_PRECHECK_TREE, default E:/mc2-installs/_precheck/tree; first use is a one-time cold build)")
    ap.add_argument("--no-receipt-check", action="store_true", help="--batch: merge tips without a green --precheck receipt (owner override; tests)")
    ap.add_argument("--no-link", action="store_true", help="with --precheck: skip the link even when the tip changes compiled inputs")
    ap.add_argument("--no-sim-rows", action="store_true", help="with --precheck: skip the saveload/determinism rows a sim-sensitive tip requires (recorded in the receipt)")
    ap.add_argument("--sim-install", help="with --precheck: install holding the tip's exe for the sim rows (default E:/mc2-installs/<laneN>)")
    ap.add_argument("--pre-gate", action="store_true",
                    help="at the merged HEAD run regenerate-all (dry), check-background-launch, gate_gen --check, cxx17 and a no-game headless "
                         "run_smoke row; all run, one PRE-GATE PASS/FAIL line; --root picks the tree")
    ap.add_argument("--exe", help="with --pre-gate: exe for the headless row (default <root>/build64-ninja/mc2.exe, skipped if absent)")
    ap.add_argument("--batch", nargs="+", metavar="TIP",
                    help="merge these tips in order; baseline follow-ups after each, build + doctest verify once at the end")
    ap.add_argument("--message", help="merge commit subject; with --batch a template: {subject} = the tip's subject, {n} = its position")
    ap.add_argument("--trailer", help="commit trailer line (default $MC2_COMMIT_TRAILER (or $MC2_MERGE_TRAILER), else none)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--regenerate-all", action="store_true",
                    help="regenerate artifacts without merging; --dry-run checks committed HEAD in an isolated clone")
    ap.add_argument("--root", default=os.path.abspath(os.path.join(HERE, "..")))
    ap.add_argument("--tools-dir")
    ap.add_argument("--resolution", action="append", default=[], metavar="PATH=FILE",
                    help="PATH=FILE, or TIP:PATH=FILE in --batch; reviewed conflict result; FILE '-' preserves a deletion (repeatable)")
    ap.add_argument("--verify", choices=["build", "full", "none"], default="build",
                    help="what to run on the merged tree: build + doctest (default), full = also the contracts board, none")
    ap.add_argument("--no-verify", action="store_true", help="same as --verify none")
    return ap


def print_plan(root: str, p: dict, args, message: str, resolutions: dict, head_ref: str) -> tuple[bool, str | None]:
    """Print the dry-run plan of one tip. -> (would_abort, virtual head after this tip or None when it cannot continue)."""
    print(f"PLAN lane={p['sha'][:9]} head={p['head'][:9]} base={p['base'][:9]} "
          f"lane_files={len(p['lane_files'])} nifty_files={len(p['nifty_files'])} "
          f"overlap={len(p['overlap'])}")
    if p["contained"]:
        print("PLAN lane tip is already contained in HEAD: nothing to merge")
        return False, head_ref
    if p["overlap"]:
        raise Stop(4, "lane files are also dirty in the worktree:\n  " + "\n  ".join(p["overlap"]))
    tree, conf = virtual_merge(root, p["sha"], head_ref)
    would_abort = False
    if conf is None:
        print("PLAN conflicts: unknown (git merge-tree unavailable)")
    else:
        floor_only = TEST_GATE in conf and test_gate_only_floor(root, p["sha"], head_ref)
        def takeable(c):
            return is_regenerable(c) or (floor_only and c == TEST_GATE)
        regen = [c for c in conf if takeable(c) or c in BUG_TABLES]
        other = [c for c in conf if not takeable(c) and c not in BUG_TABLES and c not in resolutions]
        print(f"PLAN conflicts={len(conf)} regenerable={len(regen)} blocking={len(other)}"
              + ("" if not other else " (merge would abort): " + ", ".join(other)))
        would_abort = bool(other)
    print(f'PLAN merge --no-ff -m "{message}"')
    tree = resolved_tree(root, tree, resolutions)
    vh = virtual_head(root, head_ref, p["sha"], tree)
    return would_abort, vh or (head_ref if conf is None else None)


def plan_batch(root: str, args, refs: list[str], resolutions_for) -> bool:
    """Print the plan of every tip, each against the virtual result of the earlier ones (a conflicted tip's tree is used as is, so
    later tips are planned approximately). -> True when some tip would abort."""
    would_abort, head_ref, blocked = False, "HEAD", False
    for i, ref in enumerate(refs, 1):
        p = plan(root, ref, head_ref)
        resolutions = resolutions_for(p)
        subject = tip_subject(root, p["sha"])
        message = render_message(args.message, subject, i) if args.message else f"merge lane: {subject}"
        if blocked:
            print("PLAN (approximate: planned against the conflicted tree of an earlier tip)")
        abort, head_ref = print_plan(root, p, args, message, resolutions, head_ref)
        would_abort = would_abort or abort
        blocked = blocked or abort
        if head_ref is None:
            print("PLAN the remaining tips cannot be planned: git produced no tree for this one")
            break
    return would_abort


def merge_tip(root: str, tools_dir: str, p: dict, message: str, resolutions: dict, *, batch=False) -> tuple[str, str | None, list]:
    """Merge one planned tip, regenerate the ratchets, commit the tightened baselines. -> (merge sha, follow-up sha, failed ratchets)."""
    require_clean_generator_inputs(root)
    pre_dirty = {path for _, path in status_entries(root)}
    do_merge(root, p["sha"], message, lane_files=p["lane_files"], resolutions=resolutions)
    merge_sha = git_out(root, "rev-parse", "--short=9", "HEAD")
    if batch:
        results = run_ratchets(root, tools_dir, tighten=False)
        return merge_sha, None, [(label, rc, tail) for label, rc, tail in results if rc != 0]
    artifacts = regenerate_all(root)
    artifact_follow = None
    if artifacts:
        try:
            artifact_follow = _git_util.commit_paths(root, artifacts, ARTIFACT_SUBJECT, TRAILER or None)
            print(f"lane_merge: {ARTIFACT_SUBJECT}: {artifact_follow}")
        except _git_util.GitError as exc:
            raise Stop(6, "artifact follow-up commit failed: " + str(exc)) from exc
    results = run_ratchets(root, tools_dir)
    follow = commit_tightened(root, pre_dirty)
    return merge_sha, follow or artifact_follow, [(label, rc, tail) for label, rc, tail in results if rc != 0]


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.lint:
        return lint(args.root, args.lint)
    if args.precheck:
        return precheck(args.root, args.precheck, True if args.with_link else False if args.no_link else None,
                        not args.no_sim_rows, args.sim_install)
    if args.pre_gate:
        return pre_gate(args.root, args.exe)
    if not args.regenerate_all and bool(args.ref) == bool(args.batch):
        ap.error("give one lane tip, or --batch with several")
    if args.regenerate_all and (args.ref or args.batch):
        ap.error("--regenerate-all does not take lane tips")
    refs = args.batch or [args.ref]
    batch = bool(args.batch)
    global TRAILER
    if args.trailer is not None:
        TRAILER = args.trailer
    window_started = time.time()
    root = args.root
    window_base = git_out(root, "rev-parse", "HEAD")
    tools_dir = args.tools_dir or os.path.join(root, "tools")
    try:
        if args.regenerate_all:
            regenerate_all(root, dry_run=args.dry_run)
            return 0
        resolution_specs = {}
        for spec in args.resolution:
            tip, sep, value = spec.partition(":")
            if batch:
                if not sep or tip not in refs:
                    ap.error("batch --resolution needs TIP:PATH=FILE (TIP must be in --batch)")
                key = git_out(root, "rev-parse", tip + "^{commit}")
            else:
                key, value = "single", spec
            resolution_specs.setdefault(key, []).append(value)

        def resolutions_for(p, head_ref=None):
            resolutions = load_resolutions(resolution_specs.get(p["sha"] if batch else "single", []))
            head_ref = head_ref or p["head"]
            if resolutions:
                predicted = predicted_conflicts(root, p["sha"], head_ref)
                if predicted is None or set(resolutions) - set(predicted):
                    raise Stop(5, "resolution paths must be predicted conflicts at this HEAD")
            return resolutions

        if args.dry_run:
            would_abort = plan_batch(root, args, refs, resolutions_for)
            print("PLAN then: " + ", ".join(label for label, _, _ in RATCHETS)
                  + "; regenerate-all and commit changed artifacts; follow-up commit of tightened baselines only"
                  + (" (check each tip; tighten/regenerate/commit/verify once at the end)" if batch else ""))
            print("[dry-run] nothing changed")
            if would_abort:
                print("lane_merge: the merge would abort on the blocking conflicts above", file=sys.stderr)
            return 5 if would_abort else 0
        if batch and not (args.no_receipt_check or os.environ.get("MC2_NO_RECEIPT_CHECK")):
            missing = missing_receipts(root, refs)
            if missing:
                for sha in missing:
                    print(f"PRECHECK MISSING {sha} (run tools/lane_merge.py --precheck on exactly this tip; receipts in {PRECHECK_RECEIPTS})")
                raise Stop(5, "BATCH REFUSED: no green precheck receipt; nothing was merged")
        if batch and plan_batch(root, args, refs, resolutions_for):
            raise Stop(5, "BATCH REFUSED: a tip would abort on the blocking conflict(s) above; nothing was merged")
        merged = []        # (merge sha, follow-up sha, files) per merged tip
        reds = []          # (ref, failed ratchets) per tip whose ratchets failed
        stopped = None     # first red ref; a red batch skips the build and exits 6
        for idx, ref in enumerate(refs, 1):
            p = plan(root, ref)
            resolutions = resolutions_for(p)
            if p["contained"]:
                print(f"MERGED {p['head'][:9]} lane tip already contained files=0 ratchets=SKIPPED overlap=0")
                continue
            if p["overlap"]:
                raise Stop(4, "lane files are also dirty in the worktree; no merge attempted:\n  "
                           + "\n  ".join(p["overlap"]))
            subject = tip_subject(root, p["sha"])
            message = (render_message(args.message, subject, idx) if batch else args.message) if args.message else f"merge lane: {subject}"
            merge_sha, follow, failed = merge_tip(root, tools_dir, p, message, resolutions, **({'batch': True} if batch else {}))
            merged.append((merge_sha, follow, len(p["lane_files"])))
            tag = f"MERGED {merge_sha}" + (f" (+{follow})" if follow else "") + f" files={len(p['lane_files'])}"
            if failed:
                for label, rc, tail in failed:
                    print(f"lane_merge: tip {ref} ratchet {label} exited {rc}; last lines:\n{tail}", file=sys.stderr)
                if not batch:
                    print(f"{tag} ratchets=FAIL verify=SKIPPED overlap=0")
                    return 6
                # Keep merging: the owner sees every ratchet red of the batch at once (one fix-forward, not one per tip).
                reds.append((ref, failed))
                print(f"{tag} ratchets=FAIL verify=DEFERRED overlap=0")
                continue
            if batch and ref != refs[-1]:
                print(f"{tag} ratchets=OK verify=DEFERRED overlap=0")
        if batch and merged and not reds:
            pre_dirty = {path for _, path in status_entries(root)}
            artifacts = regenerate_derived(root, window_base)
            results = run_ratchets(root, tools_dir)
            failed = [(label, rc, tail) for label, rc, tail in results if rc != 0]
            if failed:
                reds.append((refs[-1], failed))
            else:
                follow = commit_tightened(root, pre_dirty, artifacts)
                merge_sha, _, count = merged[-1]
                merged[-1] = (merge_sha, follow, count)
        if reds:
            stopped = reds[0][0]
            print(f"BATCH RATCHET REDS in {len(reds)} of {len(refs)} tip(s), merged={len(merged)}:")
            for ref, failed in reds:
                print(f"  {ref}: " + ", ".join(f"{label}(rc={rc})" for label, rc, _ in failed))
        if not merged and not batch:
            return 0
        mode = "none" if args.no_verify else args.verify
        record = dict(base=window_base, head=git_out(root, "rev-parse", "HEAD"), started=window_started,
                      tree=git_out(root, "rev-parse", "HEAD^{tree}"))
        record_path = Path(root) / "build64-ninja/merge-window.json"
        if batch and merged and not stopped:
            qfailed = quick_rows(root)
            if qfailed:
                for label, rc, tail in qfailed:
                    print(f"lane_merge: quick row {label} exited {rc}; last lines:\n{tail}", file=sys.stderr)
                print(f"BATCH STOPPED at quick rows ({', '.join(q[0] for q in qfailed)}) merged={len(merged)} remaining=(none)")
                return 6
        if stopped:
            verdict, vresults = "SKIPPED(ratchet-failure)", []
        else:
            verdict, vresults = verify_merged(root, mode)
            if batch and verdict == "OK":
                fresh, why = rebuild_if_head_moved(root)
                if not fresh:
                    print("lane_merge: " + why, file=sys.stderr)
                    verdict, vresults = "FAIL", [("exe-fingerprint", 1, why)]
        merge_sha, follow, _ = merged[-1] if merged else (git_out(root, "rev-parse", "--short=9", "HEAD"), None, 0)
        record.update(verify=verdict, merge_seconds=time.time() - window_started)
        if Path(root, "scripts/ninja_build.py").is_file():
            record_path.parent.mkdir(parents=True, exist_ok=True)
            record_path.write_text(json.dumps(record), encoding="utf-8")
            print(f"lane_merge: elapsed={record['merge_seconds']:.1f}s base={window_base}; gate continues this window clock")
        print(f"MERGED {merge_sha}" + (f" (+{follow})" if follow else "") + f" files={sum(m[2] for m in merged)} "
              f"ratchets={'FAIL' if stopped else 'OK'} verify={verdict} overlap=0" + (f" batch={len(merged)}" if batch else ""))
        if verdict == "FAIL":
            for label, rc, tail in vresults:
                if rc:
                    print(f"lane_merge: verify step {label} exited {rc}; last lines:\n{tail}", file=sys.stderr)
            return 7
        return 6 if stopped else 0
    except Stop as e:
        print(f"lane_merge: {e}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
