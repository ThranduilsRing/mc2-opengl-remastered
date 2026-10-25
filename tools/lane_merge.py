#!/usr/bin/env python3
"""Merge a lane tip into the current branch (nifty) and re-tighten the ratchets, fail-closed.

WHY: lane merges follow the same ceremony every time -- check the lane does not collide
with uncommitted work in this shared worktree, merge --no-ff, regenerate the ratchet
baselines that always conflict, re-run the ratchets at the merged HEAD, and commit only
the baselines that tightened. Doing it by hand risks sweeping another lane's dirty or
staged files into the merge commit.

Usage:
  py -3 tools/lane_merge.py <sha-or-branch> [--message "..."] [--dry-run]
  py -3 tools/lane_merge.py --batch A B C [--dry-run]   several tips in a row: ratchets regenerated and committed after EACH merge,
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
A --batch run first plans every tip (a tip that would abort refuses the whole batch, exit 5, nothing merged); a ratchet failure mid-batch prints
BATCH STOPPED at <ref> merged=<n> remaining=<refs>, skips the build and exits 6.
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

#: Commit trailer line(s): --trailer or $MC2_COMMIT_TRAILER (or $MC2_MERGE_TRAILER); none by default (the merging model names itself).
TRAILER = os.environ.get("MC2_COMMIT_TRAILER") or os.environ.get("MC2_MERGE_TRAILER", "")
FOLLOWUP_SUBJECT = "merge follow-up: ratchet baselines tightened at merged HEAD"
ARTIFACT_SUBJECT = "merge follow-up: regenerated artifacts"

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
    "tools/dup_lines_ratchet.baseline.json",
    "tools/axis_swizzle_ratchet.baseline.json",
    "scripts/gl_shader_variants.json",
}
REGENERABLE.update(p for p in ARTIFACTS if p.endswith((".json", ".md")))


def is_regenerable(path):
    return path in REGENERABLE or any(path.startswith(scope + "/") for scope in ARTIFACTS)
# The baselines the follow-up commit may contain (never a glob over tools/).
TIGHTENED_BASELINES = [
    "tools/file_size_ratchet.baseline.json",
    "tools/cxx17_ratchet/baseline.json",
    "tools/dup_lines_ratchet.baseline.json",
    "tools/axis_swizzle_ratchet.baseline.json",
]
# (label, script relative to the tools dir, args)
RATCHETS = [
    ("file_size", "file_size_ratchet.py", ["--check", "--tighten", "--worktree"]),
    ("cxx17", "cxx17_ratchet/cxx17_ratchet.py", ["--check", "--tighten", "--worktree"]),
    ("dup_lines", "dup_lines_ratchet.py", ["--check", "--tighten", "--worktree"]),
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
    return bool(re.search(r"FAILED|FAIL:|ERROR|VIOLATION|test case:|TEST CASE:|\.(?:py|cpp|h)[:(]\d+", line))


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


def compiled_inputs_changed(root: str, pre: str) -> bool:
    r = git(root, "diff", "--name-only", pre, "HEAD")
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
    foreign = [p for p in conflicted if not is_regenerable(p)]
    if foreign or not (conflicted or tables or resolutions):
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
    return git(root, "add", "--", path).returncode == 0


def run_ratchets(root: str, tools_dir: str) -> list[tuple[str, int, str]]:
    results = []
    for label, rel, args in RATCHETS:
        script = os.path.join(tools_dir, *rel.split("/"))
        r = run(root, [sys.executable, "-B", script, *args])
        tail = "\n".join(lines(r.stdout)[-3:])
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
        return _git_util.commit_paths(root, changed, FOLLOWUP_SUBJECT, TRAILER or None)
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


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", nargs="?", help="lane tip: sha or branch")
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
        regen = [c for c in conf if is_regenerable(c) or c in BUG_TABLES]
        other = [c for c in conf if not is_regenerable(c) and c not in BUG_TABLES and c not in resolutions]
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


def merge_tip(root: str, tools_dir: str, p: dict, message: str, resolutions: dict) -> tuple[str, str | None, list]:
    """Merge one planned tip, regenerate the ratchets, commit the tightened baselines. -> (merge sha, follow-up sha, failed ratchets)."""
    require_clean_generator_inputs(root)
    pre_dirty = {path for _, path in status_entries(root)}
    do_merge(root, p["sha"], message, lane_files=p["lane_files"], resolutions=resolutions)
    merge_sha = git_out(root, "rev-parse", "--short=9", "HEAD")
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
                  + (" (after each tip; verify once at the end)" if batch else ""))
            print("[dry-run] nothing changed")
            if would_abort:
                print("lane_merge: the merge would abort on the blocking conflicts above", file=sys.stderr)
            return 5 if would_abort else 0
        if batch and plan_batch(root, args, refs, resolutions_for):
            raise Stop(5, "BATCH REFUSED: a tip would abort on the blocking conflict(s) above; nothing was merged")
        merged = []        # (merge sha, follow-up sha, files) per merged tip
        stopped = None     # (ref, failed ratchets) when a ratchet failed mid-batch
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
            merge_sha, follow, failed = merge_tip(root, tools_dir, p, message, resolutions)
            merged.append((merge_sha, follow, len(p["lane_files"])))
            tag = f"MERGED {merge_sha}" + (f" (+{follow})" if follow else "") + f" files={len(p['lane_files'])}"
            if failed:
                for label, rc, tail in failed:
                    print(f"lane_merge: tip {ref} ratchet {label} exited {rc}; last lines:\n{tail}", file=sys.stderr)
                if not batch:
                    print(f"{tag} ratchets=FAIL verify=SKIPPED overlap=0")
                    return 6
                stopped = (ref, refs[idx:])
                print(f"BATCH STOPPED at {ref} merged={len(merged)} remaining={' '.join(stopped[1]) or '(none)'}")
                break
            if batch and ref != refs[-1]:
                print(f"{tag} ratchets=OK verify=DEFERRED overlap=0")
        if not merged and not batch:
            return 0
        mode = "none" if args.no_verify else args.verify
        record = dict(base=window_base, head=git_out(root, "rev-parse", "HEAD"), started=window_started)
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
