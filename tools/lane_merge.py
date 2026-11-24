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
  --conflict-dir DIR  (--batch --dry-run only) per tip in batch order, virtually merged against the running virtual head:
              DIR/<NN>-<tip>/<path> = the conflicted file with markers, plus summary.txt (sha, paths, hunk counts, earlier tips
              touching the same file, and a heuristic "same file, both tips" list to spot duplicate fixes); exits 0
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

import argparse, json
from pathlib import Path
from collections import deque
from contextlib import nullcontext
import os
import re
import shutil, subprocess, sys, time, tempfile  # noqa: E401

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)
sys.path.insert(0, HERE)
import _git_util, lane_merge_baseline, lane_merge_conflicts, lane_merge_trust  # noqa: E402  (shared git helpers; the trusted-copy hand-off)
from lint_slots import LintSlots, cpu_masks, pin_cpus, scratch_dir, sweep_stale_scratch  # noqa: E402
from lane_precheck import PRECHECK_TREE, SCRATCH_CLONE_ARGS, Stop, TreeLock, _inside, base_rot, ensure_precheck_tree, failure_line, failure_summary, precheck_scratch_base  # noqa: E402,F401
from lane_sim_rows import SIM_ROWS, default_sim_install, run_sim_rows, sim_rows_satisfied, sim_sensitive_paths, tip_changed_paths  # noqa: E402,F401
from lane_merge_stage import STAGE_HELP, bisect, bisect_step, promote, refuse_direct_nifty, stage  # noqa: E402,F401

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
    "tests/unit/own_inventory_baseline.json",
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
    ("Vulkan ownership", "tools/vk_ownership_ledger.py", ["--write"]),
]
GENERATOR_ROWS = dict(zip((g[0] for g in GENERATORS), (
    "log_tag_census", "pointer_order_scan", "gated_layout_macros", "binding_slots",
    "shader_interface", "shader_interface", "own_inventory", "vk_ownership")))

# Files that are pure functions of the tree: on conflict the lane's copy is taken and the
# ratchet --tighten run below regenerates them against the merged result.
REGENERABLE = {
    "tools/cxx17_ratchet/baseline.json",
    "scripts/gl_shader_variants.json",
}
REGENERABLE.update(p for p in ARTIFACTS if p.endswith((".json", ".md")))


# Generated files: their content is a pure function of the tree. A conflict here is
# resolved to ours and the matching regenerator rewrites them against the merged tree.
GENERATED = {
    "Gates/gate_index.gen.h", "Gates/gate_table.json", "Gates/feature_only_rows.json",
    "RenderCore/RendererFeatureRegistry.h", "docs/flags.auto.yaml",
    "docs/split-map.md",
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
    "tools/cxx17_ratchet/baseline.json",
]
# (label, script relative to the tools dir, args)
RATCHETS = [
    ("cxx17", "cxx17_ratchet/cxx17_ratchet.py", ["--check", "--worktree"]),
    ("split_map", "split/split_map.py", ["--check"]),
]
# p31 base-baseline re-check: each baselined ratchet's baseline, by label (split_map has none).
RATCHET_BASELINES = {"cxx17": "tools/cxx17_ratchet/baseline.json"}
# Row tables where both sides append rows: merged mechanically (keep both, the lane's new rows renumbered after ours).
BUG_TABLES = {".claude/cleanup/BUGS-FOUND-FIXED.md"}
ROW = re.compile(r"^\| (\d+) \|")
UNMERGED = {"DD", "AU", "UD", "UA", "DU", "AA", "UU"}


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


# Semantic ratchets whose baselines regenerate_all rewrites (--write-baseline): they are checked against the COMMITTED baseline
# first, so a merge that adds a violation stays red instead of being blessed by the regeneration (which may then only tighten).
# cxx17 is covered by the per-tip check-only run_ratchets; the log tag census by log_tag_loosened in regenerate_derived.
SEMANTIC_RATCHETS = [
    ("pointer_order", "pointer_order_scan.py", ["--check"]),
    ("own_inventory", "own_inventory.py", ["--check"]),
]


def semantic_precheck(root: str, tools_dir: str, paths=None) -> list[tuple[str, int, str]]:
    """Check-only run of SEMANTIC_RATCHETS at the merged HEAD, before any regeneration. -> the failing (label, rc, tail)."""
    failed = []
    import contracts_cache as cc
    affected = cc.affected_rows(root, paths) if paths is not None else None
    for label, rel, args in SEMANTIC_RATCHETS:
        if affected is not None and ("pointer_order_scan" if label == "pointer_order" else label) not in affected:
            continue
        script = os.path.join(tools_dir, *rel.split("/"))
        if not os.path.isfile(script):   # the throwaway repos of the tests carry only the fake ratchets
            continue
        command = [sys.executable, "-B", script, *args]
        r = run(root, command)
        if r.returncode and not base_rot(root, git_out(root, "rev-parse", "HEAD^1"), label, command, r, run):
            failed.append((label, r.returncode, "\n".join(lines(r.stdout)[-60:])))
    return failed


def run_ratchets(root: str, tools_dir: str, *, tighten=True, rows=None) -> list[tuple[str, int, str]]:
    results = []
    for label, rel, args in (RATCHETS if rows is None else rows):
        script = os.path.join(tools_dir, *rel.split("/"))
        selected = args if tighten else [a for a in args if a != '--tighten']
        r = run(root, [sys.executable, "-B", script, *selected])
        # every ratchet always runs; a failing one reports its whole violation list, not just the last lines (W78: first hit only)
        tail = "\n".join(lines(r.stdout)[-60:] if r.returncode else lines(r.stdout)[-3:])
        results.append((label, r.returncode, tail))
    return results


def report_ratchet_debt(failed):
    """Only recognized check verdicts are debt. Invocation/discovery failures stay errors."""
    headers = ("cxx17_ratchet: FAIL -", "pointer_order_scan: FAIL", "own_inventory: FAIL", "split_map: FAIL - docs/split-map.md is stale")
    for label, rc, tail in failed:
        if rc != 1 or not any(h in tail for h in headers) or "Traceback" in tail:
            raise Stop(6, f"ratchet tool failed: {label} (rc={rc})\n{tail}")
        delta = re.search(r"\(\+\d+\)|\d+ -> \d+", tail)
        locations = re.findall(r"[\w./-]+:\d+", tail)
        print(f"RATCHET-DEBT {label} {delta.group() if delta else 'changed'} {locations[0] if locations else '(see checker output)'}\n{tail}")


def record_ratchet_baselines(root, tools_dir, ratchets):
    """cxx17 has a census baseline; split_map is generated documentation, with no baseline."""
    for label, rel, _ in ratchets:
        args = ["--write-baseline"] if label == "cxx17" else ["--write"]
        result = run(root, [sys.executable, "-B", os.path.join(tools_dir, rel), *args])
        if result.returncode:
            raise Stop(6, f"baseline generator {label} failed:\n{result.stdout}")


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
    "build": [("build", ["scripts/ninja_build.py"]), ("scripts_tests", ["scripts/run_scripts_tests.py"]), ("tools_tests", ["scripts/run_tools_tests.py"])],
    "full": [("build", ["scripts/ninja_build.py"]), ("scripts_tests", ["scripts/run_scripts_tests.py"]), ("tools_tests", ["scripts/run_tools_tests.py"]), ("board", ["scripts/check-contracts.sh"])],
}


#: Seconds-cheap contract rows a lane must see before the window (W109 found them late): (label, script relative to the root, args).
EARLY_ROWS = [("sim_include_firewall", "scripts/check_sim_include_firewall.py", []),
              ("log_tag_census", "tools/log_tag_census.py", ["--check"]), ("doc_links", "scripts/check-doc-links.py", ["--quiet"])]   # W127: a broken link


#: Seconds-cheap ownership/callers checks, run before the ~18 min build + doctest so a duplicate-ownership red shows at once.
QUICK_ROWS = [("script_callers", ["tools/callers.py", "--check"]),
              ("test_ownership", ["-m", "pytest", "scripts/tests/test_contract_coverage.py", "-q", "-p", "no:cacheprovider"])]


def quick_rows(root: str, runner=run, base=None) -> list[tuple[str, int, str]]:
    """Run QUICK_ROWS (cwd = root) on the merged tree. -> the failing [(label, rc, tail)] (empty = green)."""
    failed = []
    import contracts_cache as cc
    affected = cc.affected_rows(root, lines(git_out(root, "diff", "--name-only", base, "HEAD"))) if base else None
    for label, args in QUICK_ROWS:
        if affected is not None and ("contract_coverage" if label == "test_ownership" else label) not in affected:
            continue
        missing = [a for a in args if a.endswith(".py") and not os.path.exists(os.path.join(root, a))]
        if missing:
            if os.path.abspath(root) == os.path.abspath(os.path.join(HERE, "..")):   # the real repo: a renamed script must not skip silently
                failed.append((label, -1, "quick row script missing: " + ", ".join(missing)))
            continue  # a throwaway root without the script has nothing to check
        r = runner(root, [sys.executable] + args)
        stale = stale_generated(r.returncode, r.stdout)
        if stale:
            print(f"STALE-GENERATED (the merge regenerates it): {label}")
            refresh_generated(root, stale)
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


def build_pool_jobs():
    """Merge build width: the host build pool (BUILD_POOL / compile mask), not the JOBS lane-count knob.

    ninja_build.py still takes its tokens from the pool, so lanes building at the same time are respected."""
    import build_tokens
    return str(max(1, int(build_tokens.pool_size())))


def verify_env(root: str) -> dict:
    """Leave the canonical Ninja tree ready for deploy_payload.py --ninja.

    The orchestrator owns this worktree exclusively during a merge window.
    """
    return dict(os.environ, MC2_BUILD_JOBS=build_pool_jobs(), CONTRACTS_JOBS=host_jobs(), MC2_NINJA_TREE="build64-ninja", MC2_UNIT_BUILD_DIR=os.path.join(root, "build-cc-tests"))


def verify_merged(root: str, mode: str, runner=None) -> tuple[str, list[tuple[str, int, str]]]:
    """Build beside scoped Python suites; the gate owns the single native test run."""
    if mode == "none":
        return "SKIPPED", []
    from concurrent.futures import ThreadPoolExecutor
    try:
        base = json.loads(Path(root, "build64-ninja/merge-window.json").read_text())["base"]
    except (OSError, ValueError, KeyError):
        base = "HEAD^"
    def check(step):
        label, (script, *rest) = step
        path = os.path.join(root, script)
        if not os.path.exists(path):
            return None
        if label in ("scripts_tests", "tools_tests"):
            rest += ["--base", base]
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
        return label, r.returncode, tail
    steps = VERIFY_STEPS[mode]
    with ThreadPoolExecutor(max_workers=3) as pool:
        results = [r for r in pool.map(check, [s for s in steps if s[0] != "board"]) if r]
    if not results:
        return "SKIPPED", []
    if mode == "full" and all(r[1] in (0, 3) if r[0] == "tools_tests" else r[1] == 0 for r in results):
        if result := check(steps[-1]):
            results.append(result)
    return ("FAIL" if any(rc != 0 and not (label == "tools_tests" and rc == 3) for label, rc, _ in results) else "OK"), results


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

    Census and population changes are review notes; generator failures still stop."""
    derived_notes = []
    changed = list(regenerate_all(root, base=base))
    live = Path(root) / LOG_TAG_BASELINE
    old = _git_json(root, "HEAD", LOG_TAG_BASELINE)
    if old is not None and live.is_file():
        bad = log_tag_loosened(old, json.loads(live.read_text(encoding="utf-8")))
        if bad:
            derived_notes.extend("DERIVED-CHANGED census: " + item for item in bad)
            changed.append(LOG_TAG_BASELINE)
    names_now, table_old = gate_names(Path(root) / "Gates" / "gate_table.json"), _git_json(root, base, "Gates/gate_table.json")
    if names_now is not None and table_old is not None:
        retired = _git_json(root, "HEAD", RETIRED_NAMES) or []
        if isinstance(retired, dict):
            retired = retired.get("names") or list(retired)
        lost = gate_floor_loosened({g["name"] for g in table_old["gates"]}, names_now, set(retired))
        if lost:
            derived_notes.extend("DERIVED-CHANGED gates: " + item for item in lost)
        if sync_test_gate_floor(root):
            changed.append(TEST_GATE)
    if derived_notes:
        print("\n".join(derived_notes))
        record_path = Path(root) / "build64-ninja/merge-window.json"
        record_path.parent.mkdir(parents=True, exist_ok=True)
        record = json.loads(record_path.read_text(encoding="utf-8")) if record_path.exists() else {}
        record["derived_notes"] = derived_notes
        record_path.write_text(json.dumps(record), encoding="utf-8")
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
    for args in (("diff", "HEAD", "--name-only"),):
        result = _git_util.git(root, *args, "-z", "--", *ARTIFACTS)
        if result.returncode:
            raise Stop(6, "cannot list artifacts: " + result.stderr)
        changed.update(p for p in result.stdout.split("\0") if p)
    return sorted(changed)


def require_clean_generator_inputs(root):
    """Artifacts must describe committed HEAD, including inputs outside ARTIFACTS."""
    # Throwaway merge-test repos without generators do not regenerate anything.
    if os.path.exists(os.path.join(root, GENERATORS[0][1])):
        dirty = [(status, path) for status, path in status_entries(root) if status != "??"]
        if dirty:
            raise Stop(6, "regenerate-all requires a clean tree; commit or remove generator input edits first:\n  "
                       + "\n  ".join(path for _, path in dirty))


def regenerate_all(root, *, dry_run=False, base=None):
    """Regenerate the committed tree in isolation for dry runs; never bless preexisting edits."""
    if dry_run:
        # A disposable local clone lets every generator use its normal paths and
        # git inventory. No generator writes into the caller's checkout.
        proof_root = "E:/mc2-archive-2026-10/proofs" if os.path.isdir("E:/mc2-archive-2026-10/proofs") else None
        with tempfile.TemporaryDirectory(prefix="regen-", dir=proof_root) as scratch:
            snapshot = os.path.join(scratch, "tree")
            r = run(root, ["git", *SCRATCH_CLONE_ARGS, os.path.abspath(root), snapshot])
            if r.returncode:
                raise Stop(6, r.stdout)
            sha = git_out(root, "rev-parse", "HEAD")
            git_out(snapshot, "checkout", "--quiet", "--detach", sha)
            changed = regenerate_all(snapshot, base=base)
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
    # The generators write disjoint artifacts (ARTIFACTS) and none reads another's output, so they run concurrently
    # (about 4 at a time: the precheck budget is shared with the suites). Failures are reported in table order.
    from concurrent.futures import ThreadPoolExecutor

    def generate(g):
        label, script, args = g
        return label, run(root, [sys.executable, "-B", os.path.join(root, script), *args])
    # Compiled SPIR-V is toolchain-bound: the Linux box's output differs byte-wise from the committed Windows build, so it is skipped there.
    gens = [g for g in GENERATORS if os.name == 'nt' or g[0] != 'SPIR-V pilots']
    base = base or os.environ.get("MC2_GATE_BASE")
    if base:
        import contracts_cache as cc
        paths = set(lines(git_out(root, "diff", "--name-only", base, "HEAD")))
        declarations, universe = cc.load_decls(root), cc.Universe(root)
        gens = [g for g in gens if GENERATOR_ROWS.get(g[0]) not in declarations
                or paths.intersection(cc.row_inputs(root, universe, g[1], declarations[GENERATOR_ROWS[g[0]]]))
                or any(cc._match(p, s) for p in paths for s in declarations[GENERATOR_ROWS[g[0]]])]
    if not gens:
        print("regenerate-all: no declared inputs changed")
        return []
    with ThreadPoolExecutor(max_workers=min(REGEN_WORKERS, len(gens))) as pool:
        results = list(pool.map(generate, gens))
    for label, r in results:
        print(f"regenerate-all: {label}: {'OK' if r.returncode == 0 else 'FAIL'}", flush=True)
    for label, r in results:
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


def lint_rows(py: list[str], base_ref: str, ref: str) -> list[tuple[str, list[str], str]]:
    """The seconds-cheap checks a precheck would fail on: (label, command, fix hint). Same tables as precheck."""
    rows = [(label, py + [os.path.join("tools", script), *[a for a in args if a != "--tighten"]],
             "fix the code, or lower the baseline via the ratchet's own --tighten (never raise it)") for label, script, args in RATCHETS]
    rows += [("callers", py + ["tools/callers.py", "--check"], "wire the script into a caller or add it to tests/callers_allowlist.json with a reason")]
    rows += [(label, py + [script, *args], "see the row's own output") for label, script, args in EARLY_ROWS]
    rows.append(("ungated_tags", py + ["tools/ungated_log_tags.py", "--base", base_ref, "--head", ref],
                 "gate the new log tag or register it as the check says"))
    # W138d: both passed lint and failed on merged HEAD (pre_gate / the laptop gate). Same background-launch row pre_gate runs.
    rows.append(("background_launch", py + ["scripts/check-background-launch.py", "--quiet"],
                 "pass **console_child.child_kwargs() to the spawn, add the program to scripts/background-launch-allow.json, or `# background-launch: allow <why>`"))
    rows.append(("new_drive_paths", py + ["tools/new_drive_paths.py", "--base", base_ref, "--head", ref],
                 "the laptop gate has no E: drive: read the path from an env var default, or add `# drive-path: ok <why>`"))
    return rows


#: A path with a line number in a row's failure output: a file the row reports as failing.
FAIL_LOC = re.compile(r"([A-Za-z0-9_][A-Za-z0-9_.+-]*/[A-Za-z0-9_.+-]*\.[A-Za-z0-9]{1,8})[:(]\d+")


def generated_hint(tail: str) -> str | None:
    """A failing row's output names files of the GENERATED set as stale and no other file fails:
    the merge regenerates them, so say so and name the regenerator's own command. -> the hint line, or None."""
    stale = sorted({p for p in GENERATED if p in tail})
    if not stale or any(p not in GENERATED for p in FAIL_LOC.findall(tail)):
        return None
    cmds = ["py -3 " + " ".join([script, *args]) for files, script, args in GENERATED_REGENERATORS if set(stale) & files]
    return ("only generated files are stale (" + ", ".join(stale) + "); "
            "the merge regenerates them, or run " + " / ".join(cmds))


def stale_generated(rc, output):
    """Exact stale-output diagnostics only; unknown or mixed failures never qualify."""
    if rc != 1:
        return set()
    paths = set()
    for line in lines(output):
        line = line.strip()
        if re.fullmatch(r"\[background-launch\] FAIL \(\d+ problem\(s\)\):|GATE-TABLE check FAILED", line):
            continue
        line = line.removeprefix("- ")
        match = re.fullmatch(r"(docs/background-launch-inventory\.md): stale\. Re-run with --write\.", line)
        gate = re.fullmatch(r"(Gates/gate_index\.gen\.h|RenderCore/RendererFeatureRegistry\.h|docs/flags\.auto\.yaml)(?: is| rows are) stale against (?:Gates/gate_table\.json|the gate table): run py -3 tools/gate_gen\.py --write", line)
        if match or gate:
            paths.add((match or gate)[1])
        elif line == "split_map: FAIL - docs/split-map.md is stale (a split piece was added, removed or re-described); run: py -3 tools/split/split_map.py --write":
            paths.add("docs/split-map.md")
        else:
            return set()
    return paths


def refresh_generated(root, paths):
    for files, script, args in GENERATED_REGENERATORS:
        if set(paths) & files:
            result = run(root, [sys.executable, "-B", os.path.join(root, script), *args])
            if result.returncode:
                raise Stop(6, f"generator {script} failed:\n{result.stdout}")


def report_lint_row(label: str, rc: int, secs: float, tail: str, hint: str) -> None:
    """One lint row's report: the verdict line, and for a red the failure tail, its fix and the generated-file hint."""
    print(f"lint: {label}: {'PASS' if rc == 0 else 'FAIL'} ({secs:.0f}s)", flush=True)
    if rc:
        print("  " + tail.replace("\n", "\n  "), flush=True)
        if hint:
            print("  fix: " + hint, flush=True)
        if (note := generated_hint(tail)) is not None:
            print("  " + note, flush=True)


REGEN_WORKERS = 4   # regenerate_all's inner pool; counted against a lint's CPU budget


def merge_target(root: str, base: str | None = None) -> str:
    """Explicit target, active play branch, documented play branch, then legacy nifty."""
    if base:
        return git_out(root, "rev-parse", base + "^{commit}")
    branch = git(root, "symbolic-ref", "--quiet", "--short", "HEAD").stdout.strip()
    if branch.startswith("claude/play-"):
        return git_out(root, "rev-parse", branch + "^{commit}")
    guide = Path(root, "CLAUDE.md")
    if guide.is_file():
        match = re.search(r"\bclaude/play-[\w.-]+", guide.read_text(encoding="utf-8"))
        if match:
            return git_out(root, "rev-parse", match[0] + "^{commit}")
    return git_out(root, "rev-parse", "claude/nifty-mendeleev^{commit}")


def lint(root: str, ref: str, runner=None, base: str | None = None, *, stale_paths=None) -> int:
    """One host slot per lint run (granted FIFO by LintSlots), after sweeping dead lints' scratch."""
    sweep_stale_scratch([precheck_scratch_base(root), "E:/mc2-archive-2026-10/proofs"])
    hold = LintSlots().acquire()   # host-wide JOBS cap across all `--lint` processes, released when this run ends
    try:
        return _lint_locked(root, ref, runner, base, **({"stale_paths": stale_paths} if stale_paths is not None else {}))
    finally:
        hold.close()


def _lint_locked(root: str, ref: str, runner=None, base: str | None = None, *, stale_paths=None) -> int:
    """`--lint REF`: the precheck's fast local checks (ratchets, callers, early rows, ungated tags)
    run concurrently at REF in shared clones; every red is printed with its fix. No Python suites, no link. Exit 0 = clean."""
    real_runner = runner is None
    runner, started, failed = runner or run, time.monotonic(), []   # type: ignore[var-annotated]
    import contracts_cache as cc
    receipt_tip, rotations = cc.clean_tip(root), []
    if real_runner:
        cc.record_tip_pass(root, "lint", receipt_tip, False)
    try:
        sha = git_out(root, "rev-parse", ref + "^{commit}")
        base_ref = merge_target(root, base)
        base_ref = git_out(root, "merge-base", sha, base_ref)
    except Stop as exc:
        print("LINT FAIL " + str(exc))
        return 1
    rows = lint_rows([sys.executable, "-B"], base_ref, sha)
    chains = [[r] for r in rows]   # every row is independent: concurrent
    # lint never takes the precheck TreeLock (own shared clones)
    with scratch_dir("lane-lint-", precheck_scratch_base(root)) as scratch:
        snap = os.path.join(scratch, "tree")   # one checkout shared by all rows
        r = run(root, ["git", *SCRATCH_CLONE_ARGS, os.path.abspath(root), snap])
        if r.returncode:
            print("LINT FAIL clone failed: " + failure_summary(r.stdout))
            return 1
        git_out(snap, "checkout", "--quiet", "--detach", sha)

        def chain(i):
            out = []
            for label, cmd, hint in chains[i]:
                t0 = time.monotonic()
                r = runner(snap, cmd)
                rot = base_rot(snap, base_ref, label, cmd, r, runner)
                if rot:
                    rotations.append(label)
                stale = stale_generated(r.returncode, r.stdout)
                if stale:
                    if stale_paths is not None:
                        stale_paths.update(stale)
                    print(f"STALE-GENERATED (the merge regenerates it): {label}")
                out.append((label, 0 if rot or stale else r.returncode, time.monotonic() - t0, failure_summary(r.stdout), hint))
            return out
        from concurrent.futures import ThreadPoolExecutor
        row_workers = max(1, min(len(chains), int(host_jobs()), bin(cpu_masks()["lane"]).count("1")))
        with ThreadPoolExecutor(max_workers=row_workers) as pool:
            for f in [pool.submit(chain, i) for i in range(len(chains))]:
                try:
                    results = f.result()
                except (Stop, OSError) as exc:
                    failed.append("lint:" + str(exc).replace("\n", " "))
                    continue
                for label, rc, secs, tail, hint in results:
                    if rc:
                        failed.append(label)
                    report_lint_row(label, rc, secs, tail, hint)
    print("LINT " + ("FAIL " + ", ".join(failed) if failed else "PASS") + f" WALL={time.monotonic() - started:.0f}s (the precheck still runs the Python suites and the link)")
    if real_runner:
        cc.record_tip_pass(root, "lint", receipt_tip, not failed and not rotations and receipt_tip == sha)
    return int(bool(failed))


def batch_lint_ok(root: str, run=None) -> bool:
    if run:
        return run(root, "HEAD") == 0
    stale = set()
    rc = lint(root, "HEAD", stale_paths=stale)
    if stale:
        refresh_generated(root, stale)
        commit_tightened(root, set(), GENERATED)
        pending = set()
        rc = lint(root, "HEAD", stale_paths=pending)
        return rc == 0 and not pending
    return rc == 0


PRECHECK_RECEIPTS = os.environ.get("MC2_PRECHECK_RECEIPTS", "A:/Games/mc2-ops/precheck")


def receipt_path(sha: str) -> str:
    if not re.fullmatch(r"[0-9a-f]{40}", sha or ""):   # a mocked or truncated rev-parse must never forge a receipt file
        raise ValueError(f"precheck receipt needs a full 40-hex commit SHA, got {sha!r}")
    return os.path.join(PRECHECK_RECEIPTS, sha + ".json")


def write_receipt(root: str, tip: str, with_link, verdict: str, sim_rows: dict | None = None) -> None:
    """An informational receipt for this exact tip; sim_rows records required only."""
    sha = git_out(root, "rev-parse", tip + "^{commit}")
    os.makedirs(PRECHECK_RECEIPTS, exist_ok=True)
    body = {"tip": sha, "base_head": git_out(root, "rev-parse", "HEAD"), "base_tree": git_out(root, "rev-parse", "HEAD^{tree}"),
            "verdict": verdict, "link": bool(with_link), "when": time.time()}
    if sim_rows is not None:
        body["sim_rows"] = sim_rows
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=PRECHECK_RECEIPTS, delete=False, encoding="utf-8") as f:
            tmp = f.name
            json.dump(body, f)
        os.replace(tmp, receipt_path(sha))
    finally:
        if tmp and os.path.exists(tmp):
            os.unlink(tmp)


def missing_receipts(root: str, refs: list[str]) -> list[str]:
    out = []
    for ref in refs:
        sha = git_out(root, "rev-parse", ref + "^{commit}")
        try:
            with open(receipt_path(sha), encoding="utf-8") as f:
                receipt = json.load(f)
            ok = receipt.get("verdict") == "PASS"
        except (OSError, ValueError):
            ok = False
        if not ok:
            out.append(sha)
    return out


def precheck(root: str, tip: str, with_link: bool | None = None, sim_rows: bool = True, sim_install: str | None = None,
             sim_runner=None, sim_quiet=None, base: str | None = None) -> int:
    """Grade the virtual nifty merge, including Python suites, without merging it.

    Checks (minutes): conflict check, 5 ratchets, callers refresh+check and Python suites in disposable shared
    clones outside the main worktree. --with-link also runs, concurrently, an incremental configure + link of mc2, mc2fx,
    mc2fx_preview in the persistent warm tree (PRECHECK_TREE); only that build holds its exclusive lock.
    Only explicit --with-link enables linking; sim rows run once in the merge gate."""
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
            # Two suites (+ the link) share the host budget instead of each taking all of JOBS (about 18 processes).
            env["MC2_TEST_JOBS"] = str(max(1, int(host_jobs()) // (3 if with_link else 2)))
        r = run(workdir, command, env=env)
        secs = time.monotonic() - t0
        timings.append((label, secs))
        rot = r.returncode not in ok and base_rot(workdir, base_ref, label, command, r, run, env)
        word = "BASE-ROT" if rot else "KNOWN" if r.returncode == 3 and 3 in ok else "PASS" if r.returncode == 0 else "FAIL"
        print(f"precheck: {label}: {word} ({secs:.0f}s)", flush=True)
        if r.returncode not in ok and not rot:
            failed.append(label + ":" + failure_summary(r.stdout).replace("\n", " "))

    try:
        base_ref = merge_target(root, base)
        def virtual():
            base = base_ref
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
        scope_base = git_out(root, "merge-base", tip, base_ref)
        with_link = bool(with_link)
        py = [sys.executable, "-B"]
        steps = [(label, py + [os.path.join("tools", script), *[a for a in args if a != "--tighten"]])
                 for label, script, args in RATCHETS]
        steps += [("callers", py + ["tools/callers.py", "--check"])]
        steps += [(label, py + [script, *args]) for label, script, args in EARLY_ROWS]
        steps += [(label, py + args + ["--base", base_ref]) for label, args in PYTHON_SUITES]
        import contracts_cache as cc
        paths = lines(git_out(root, "diff", "--name-only", base_ref, commit))
        affected = cc.affected_rows(root, paths)
        aliases = {"cxx17": "cxx17_ratchet", "callers": "script_callers"}
        steps = [s for s in steps if s[0] in {p[0] for p in PYTHON_SUITES}
                 or aliases.get(s[0], s[0]) in affected]

        if "log_tag_census" in affected:
            stage("ungated_tags", root, py + [os.path.join(root, "tools", "ungated_log_tags.py"), "--base", scope_base, "--head", tip])

        # proof-diet: generated inventories are no precheck proof; the merge commit regenerates them once.

        def build_job():
            # The warm tree lock covers only checkout + configure + link; ratchets, callers and pytest run in shared
            # clones, so a queued precheck waits for a build (minutes), never for another precheck's whole run (W113: 1629 s).
            with TreeLock(PRECHECK_TREE + ".lock"):
                timed("tree-checkout", lambda: ensure_precheck_tree(root, PRECHECK_TREE, commit))
                stage("configure", PRECHECK_TREE, py + ["scripts/configure_ninja.py", "--build-dir", "build64-ninja"])
                link_j = str(max(1, int(host_jobs()) // 2))   # suites take JOBS/3 each; the sum stays near JOBS
                stage("link-mc2", PRECHECK_TREE, py + ["scripts/ninja_build.py", "mc2", "-j" + link_j])
                stage("link-tools", PRECHECK_TREE, py + ["scripts/ninja_build.py", "mc2fx", "mc2fx_preview", "-j" + link_j])

        # Independent chains run concurrently, each in its own shared clone (no shared generated files).
        chains = [[s for s in steps if s[0] in {r[0] for r in RATCHETS}],
                  [s for s in steps if s[0].startswith("callers") or s[0] in {r[0] for r in EARLY_ROWS}]] + [[s] for s in steps if s[0] in {p[0] for p in PYTHON_SUITES}]
        with tempfile.TemporaryDirectory(prefix="lane-precheck-", dir=precheck_scratch_base(root)) as scratch:
            def chain(i):
                snapshot = os.path.join(scratch, f"tree{i}")
                t0 = time.monotonic()
                r = run(root, ["git", *SCRATCH_CLONE_ARGS, os.path.abspath(root), snapshot])
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
    sim = {"required": False}
    try:
        sim["required"] = bool(sim_sensitive_paths(tip_changed_paths(root, tip, merge_target(root, base))))
    except (Stop, OSError, KeyError, ValueError, RuntimeError) as exc:
        failed.append("sim-rows: cannot classify the tip's paths: " + str(exc).replace("\n", " "))
    if sim["required"]:
        print("precheck: sim rows required at merge gate (receipt only)", flush=True)
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


#: The missions whose committed baselines pin the install data fingerprint (the determinism/saveload gate rows).
FINGERPRINT_MISSIONS = ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24")


def recorded_data_fingerprint(root: str) -> dict | None:
    """The data_fingerprint the determinism/saveload baselines pin (the recorded baseline), or None when none records one
    or they disagree (a baseline-regeneration problem, not a pre-gate verdict)."""
    import sim_determinism as sd
    seen = {}
    for mission in FINGERPRINT_MISSIONS:
        try:
            with open(sd.baseline_path(mission), encoding="utf-8") as fh:
                fp = json.load(fh).get("data_fingerprint")
        except (OSError, ValueError):
            continue
        if fp and fp.get("digest"):
            seen.setdefault(fp["digest"], fp)
    return next(iter(seen.values())) if len(seen) == 1 else None


def candidate_data_fingerprint(root: str) -> tuple[dict | None, str]:
    """The data fingerprint the gate would compute for this tree: the stock reference install (the gate's deploy target,
    sim_determinism.INSTALL_DIR) read with the candidate tree's declared overlay manifest (its tests/smoke/sim_determinism/
    data_overlay.json). -> (fingerprint, skip_reason): fingerprint is None, with a one-line reason, when there is no stock
    install to read. Read-only: it walks and hashes the install but never writes to it. The stock parts are gitignored and
    absent from a source tree, so the source tree itself can never be fingerprinted -- only the install the gate deploys to."""
    import sim_determinism as sd
    install = sd.INSTALL_DIR
    if not os.path.isdir(install):
        return None, f"no stock reference install at {install} to fingerprint"
    manifest = sd.load_overlay_manifest(os.path.join(root, "tests", "smoke", "sim_determinism", "data_overlay.json"))
    return sd.data_fingerprint(install, manifest), ""


def differing_stock_parts(fp: dict, base: dict) -> str:
    """The stock parts that differ between the candidate fingerprint and the baseline (the FAIL detail)."""
    got, want = set(fp.get("inputs") or []), set(base.get("inputs") or [])
    parts = []
    if (fp.get("stock_digest") or "") != (base.get("stock_digest") or ""):
        parts.append(f"stock_digest {fp.get('stock_digest')} is not the baseline's {base.get('stock_digest')}")
    parts += [f"stock part {p} is absent from the install" for p in sorted(want - got)]
    parts += [f"stock part {p} is not in the baseline" for p in sorted(got - want)]
    parts += fp.get("problems") or []
    return "; ".join(parts) or "the data fingerprint digest differs"


def data_fingerprint_check(root: str, fingerprint=None, baseline=None) -> tuple[str, str]:
    """The gate's sim data fingerprint (stock reference install + the candidate tree's declared overlay) vs the recorded
    baseline the determinism/saveload rows pin. -> (verdict, detail): 'OK' when it matches, 'FAIL' when it does not (the
    gate would go red), 'SKIPPED' when there is no recorded baseline or no stock install to read. W138h: this red only
    showed on the laptop gate, costing two extra runs (52 min)."""
    fp, skip = (fingerprint or candidate_data_fingerprint)(root)
    if fp is None:
        return "SKIPPED", skip
    base = (baseline or recorded_data_fingerprint)(root)
    if not base:
        return "SKIPPED", "no recorded baseline data fingerprint"
    if fp["digest"] != base["digest"]:
        return "FAIL", differing_stock_parts(fp, base)
    return "OK", ""


def cooked_outputs_check(root: str) -> tuple[str, str]:
    """The gitignored PC-cooked outputs (tools/cooked_outputs.json: tree-upscale sidecars, grass turf, terrain layers) present and at their
    pinned bytes under `root`. -> (verdict, detail): 'FAIL' lists each problem (the gate's compare-captures and the laptop provision would
    otherwise fail on it, W138h x4 / W147 run 4), 'SKIPPED' when `root` has no manifest (not a repo tree)."""
    if not os.path.isfile(os.path.join(root, "tools", "cooked_outputs.json")):
        return "SKIPPED", f"no tools/cooked_outputs.json under {root}"
    sys.path.insert(0, os.path.join(root, "tools"))
    import cooked_outputs
    bad = cooked_outputs.problems(root)
    return ("FAIL", "\n".join(bad)) if bad else ("OK", "")


#: pre-gate step label -> the contracts-board row (scripts/check-contracts.sh run_check/run_nightly label) it mirrors.
PRE_GATE_CONTRACT_ROWS = {"gate_gen": "gate_gen_check", "cxx17": "cxx17_ratchet", "sim_include_firewall": "sim_include_firewall",
                          "log_tag_census": "log_tag_census", "doc_links": "doc_links", "stock_mission_data": "stock_mission_data"}
#: gate rows (scripts/merge_gate.py plan) whose INPUTS a pre-gate row checks, though the row itself needs the laptop.
PRE_GATE_INPUT_ROWS = {"deploy": "exe-fingerprint", "capture-before": "cooked_outputs", "capture-after": "cooked_outputs",
                       "compare-captures": "cooked_outputs", "determinism-": "data_fingerprint", "saveload-": "data_fingerprint"}


def contracts_row_command(root: str, label: str) -> str | None:
    """The command string of contracts-board row `label` (first run_check/run_nightly line naming it), or None."""
    try:
        text = Path(root, "scripts", "check-contracts.sh").read_text(encoding="utf-8")
    except OSError:
        return None
    m = re.search(r'^run_(?:check|nightly)\s+"%s"\s+"((?:[^"\\]|\\.)*)"' % re.escape(label), text, re.M)
    return m.group(1) if m else None


def command_parity(root: str, step_cmd: list[str], contracts_cmd: str | None) -> str:
    """SAME when the pre-gate step runs the contracts row's command (first `&&` segment: interpreter flags and absolute paths aside),
    PARTIAL when the row has more segments after it, DIFFERS (naming the argument delta) otherwise."""
    if contracts_cmd is None:
        return "no contracts row"
    def norm(tokens):
        out = [t for t in tokens if t not in ("py", "-3", "-B") and not t.endswith("python.exe") and t != sys.executable]
        return [os.path.relpath(t, root).replace("\\", "/") if os.path.isabs(t) else t for t in out]
    segments = [s.split() for s in contracts_cmd.split("&&")]
    pre, row = norm(step_cmd), norm(segments[0])
    if pre != row:
        return "DIFFERS: pre-gate " + " ".join(pre[1:]) + " vs contracts " + " ".join(row[1:])
    return "SAME" if len(segments) == 1 else f"PARTIAL (the row also runs {len(segments) - 1} more command(s))"


def parity_table(root: str, step_labels: list[str], steps: list | None = None, gate_rows: list[str] | None = None) -> list[str]:
    """Lines of the parity table: every laptop gate row (scripts/merge_gate.py plan), and whether this pre-gate ran the same command, only
    its inputs, or nothing. `steps` is [(label, argv)] as pre_gate built them (command comparison); `gate_rows` overrides the plan."""
    if gate_rows is None:
        try:
            sys.path.insert(0, os.path.join(root, "scripts"))
            import merge_gate
            gate_rows = [s[0] for s in merge_gate.plan(1)]
        except Exception as exc:  # noqa: BLE001  (a table failure must not decide the verdict)
            return [f"pre-gate parity: gate plan unreadable ({type(exc).__name__}: {exc})"]
    lines = ["pre-gate parity (laptop gate row | this pre-gate):"]
    cmds = dict(steps or [])
    for row in gate_rows:
        if row == "contracts":
            lines.append(f"  contracts | {len([l for l in step_labels if l in PRE_GATE_CONTRACT_ROWS])} cheap board rows only; the rest of the board NOT RUN")
            for label in step_labels:
                crow = PRE_GATE_CONTRACT_ROWS.get(label)
                if crow:
                    verdict = command_parity(root, cmds[label], contracts_row_command(root, crow)) if label in cmds else "ran"
                    lines.append(f"    {crow} | {label}: {verdict}")
            continue
        via = next((v for k, v in PRE_GATE_INPUT_ROWS.items() if row == k or (k.endswith("-") and row.startswith(k))), None)
        lines.append(f"  {row} | " + (f"NOT RUN (inputs checked by {via})" if via else "NOT RUN"))
    return lines


def pre_gate_regen(root: str) -> tuple[list[str], str | None]:
    """tooling-speed-2a: the real regenerate-all at the merged HEAD (W148 needed three separate re-freeze commits).
    Writes the regenerated artifacts into the tree and prints the exact files changed plus the one commit command.
    -> (changed files, failure text or None). A tree with no engine generators (fixtures) is skipped."""
    if not os.path.exists(os.path.join(root, GENERATORS[0][1])):
        return [], None
    try:
        pending = artifact_changes(root)
        if pending:
            return pending, ("uncommitted artifact edits already in the tree; commit them (or `git checkout -- <path>`) and rerun: "
                             + ", ".join(pending))
        changed = regenerate_all(root)
    except Stop as e:
        return [], str(e)
    if changed:
        print("pre-gate: regenerate-all changed " + str(len(changed)) + " path(s); commit them ONCE before the gate:", flush=True)
        for path in changed:
            print("  " + path, flush=True)
        print('  git add -A -- ' + " ".join(changed) + ' && git commit -m "merge fix: regenerate-all at the merged HEAD"', flush=True)
    return changed, None


def pre_gate(root: str, exe: str | None = None, nightly_proofs: str | None = None) -> int:
    """Everything that used to fail only after the gate started (W79: three reruns), run in order at the merged HEAD.
    Every step runs; one verdict line at the end. Exit 0 = clean (regenerated files are listed, not a failure). Nightly freshness is a WARN line only (it used to surface
    at promote, lane_merge_stage.untriaged_nightly_fail); the owner does not gate on it here."""
    py = [sys.executable, "-B"]
    failed = []
    # First, so the checks below see the regenerated artifacts and a stale gate_gen/log-tag table is not reported twice.
    regen_changed, regen_fail = pre_gate_regen(root)
    print(f"pre-gate: regenerate-all: {'FAIL' if regen_fail else 'OK'}", flush=True)
    if regen_fail:
        print(regen_fail, flush=True)
        failed.append("regenerate-all")
    steps = [
        ("background-launch", py + [os.path.join(root, "scripts", "check-background-launch.py"), "--quiet"]),
        # --rebuild: the form the laptop contracts row runs (it compiles tools/gate_dump and diffs its JSON against the C++ defaults;
        # W147 run 3: MC2_VEGETATION_MAX json 5M vs C++ 10M passed here without it and failed there).
        ("gate_gen", py + [os.path.join(root, "tools", "gate_gen.py"), "--check", "--rebuild"]),
        ("cxx17", py + [os.path.join(root, "tools", "cxx17_ratchet", "cxx17_ratchet.py"), "--check", "--worktree"]),
    ]
    steps += [(label, py + [os.path.join(root, script), *args]) for label, script, args in EARLY_ROWS]
    # W138h: the stock-data contract went red only on the laptop; the contracts board's own args (--no-data --quiet).
    steps += [("stock_mission_data", py + [os.path.join(root, "scripts", "check-stock-mission-data.py"), "--no-data", "--quiet"])]
    exe = exe or os.path.join(root, "build64-ninja", "mc2.exe")
    if os.path.isfile(exe) and exe_matches_head(root, os.path.relpath(os.path.dirname(exe), root)) is False:
        print("pre-gate: exe-fingerprint: FAIL\nthe built exe embeds a different sha than HEAD; rebuild (scripts/ninja_build.py) before the gate", flush=True)
        failed.append("exe-fingerprint")
    for label, cmd in steps:
        r = run(root, cmd)
        rc, out = r.returncode, "\n".join(lines(r.stdout)[-15:])
        print(f"pre-gate: {label}: {'OK' if rc == 0 else 'FAIL'}", flush=True)
        if rc:
            print(out, flush=True)
            failed.append(label)
    verdict, detail = data_fingerprint_check(root)
    print(f"pre-gate: data_fingerprint: {verdict}", flush=True)
    if detail:   # the FAIL's differing parts, or the one-line SKIPPED reason
        print(detail, flush=True)
    if verdict == "FAIL":
        failed.append("data_fingerprint")
    verdict, detail = cooked_outputs_check(root)
    print(f"pre-gate: cooked_outputs: {verdict}", flush=True)
    if detail:
        print(detail, flush=True)
    if verdict == "FAIL":
        failed.append("cooked_outputs")
    import gate_hosts
    verdict, detail = gate_hosts.editor_stage()
    print(f"pre-gate: editor-stage: {verdict} ({detail})", flush=True)
    for line in parity_table(root, [label for label, _cmd in steps], steps):
        print(line, flush=True)
    import lane_merge_stage
    stale = lane_merge_stage.untriaged_nightly_fail(nightly_proofs or lane_merge_stage.NIGHTLY_PROOFS)
    print("pre-gate: nightly: " + ("WARN (promote will refuse until fixed or triaged)" if stale else "OK"), flush=True)
    for why in stale:
        print("  " + why, flush=True)
    print("PRE-GATE " + ("PASS" if not failed else "FAIL: " + ", ".join(failed)))
    return 1 if failed else 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, epilog=STAGE_HELP, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", metavar="W", help="with --batch: build candidate claude/stage-W from nifty with the frozen tips (see the merge queue below)")
    ap.add_argument("--promote", metavar="W", help="fast-forward nifty to the claude/stage-W candidate; only on a complete PASS verdict for its HEAD")
    ap.add_argument("--hotfix", action="store_true", help="logged escape: merge directly into nifty without stage/gate/promote (recorded in the next gate verdict)")
    ap.add_argument("--verdict", help="with --promote: gate verdict JSON (default <root>/build64-ninja/gate-verdict.json)")
    ap.add_argument("--bisect", metavar="W", help="after a candidate verdict (--red / --green): halve a red batch, reject a red single tip")
    ap.add_argument("--red", action="store_true", help="with --bisect: the candidate failed")
    ap.add_argument("--green", action="store_true", help="with --bisect: the candidate passed (and was promoted)")
    ap.add_argument("ref", nargs="?", help="lane tip: sha or branch")
    ap.add_argument("--lint", metavar="REF", help="seconds-fast subset of --precheck (ratchets, callers, early rows, ungated tags) with a fix per red; run it before --precheck")
    ap.add_argument("--precheck", metavar="TIP", help="offline virtual nifty merge: conflicts, ratchets, callers, Python suites (fast, no native build; --with-link adds links)")
    ap.add_argument("--base", help="--lint/--precheck merge target (default: active/documented play branch, then nifty)")
    ap.add_argument("--with-link", action="store_true",
                    help="with --precheck: also link mc2, mc2fx, mc2fx_preview incrementally in the warm tree "
                         "($MC2_PRECHECK_TREE, default E:/mc2-installs/_precheck/tree; first use is a one-time cold build)")
    ap.add_argument("--no-receipt-check", action="store_true", help="compatibility no-op; precheck receipts are informational")
    ap.add_argument("--no-link", action="store_true", help="with --precheck: skip the link even when the tip changes compiled inputs")
    ap.add_argument("--no-sim-rows", action="store_true", help="compatibility no-op; precheck never runs games")
    ap.add_argument("--sim-install", help="compatibility no-op; precheck never runs games")
    ap.add_argument("--pre-gate", action="store_true",
                    help="at the merged HEAD run the real regenerate-all (lists the changed files; commit them once), "
                         "check-background-launch, gate_gen --check and cxx17; all run, one PRE-GATE PASS/FAIL line; --root picks the tree")
    ap.add_argument("--exe", help="with --pre-gate: exe whose fingerprint must match HEAD (default <root>/build64-ninja/mc2.exe, skipped if absent)")
    ap.add_argument("--batch", nargs="+", metavar="TIP",
                    help="merge these tips in order; baseline follow-ups after each, build + doctest verify once at the end")
    ap.add_argument("--message", help="merge commit subject; with --batch a template: {subject} = the tip's subject, {n} = its position")
    ap.add_argument("--trailer", help="commit trailer line (default $MC2_COMMIT_TRAILER (or $MC2_MERGE_TRAILER), else none)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--conflict-dir", metavar="DIR",
                    help="with --batch --dry-run: write each tip's conflicted files (markers) and a summary.txt to DIR/<NN>-<tip>/; changes nothing else")
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
    import contracts_cache as cc
    import run_tools_tests as rtt
    import run_scripts_tests as rst
    import merge_gate
    paths = p["lane_files"]
    board = Path(root, "scripts/check-contracts.sh")
    per_merge = set(re.findall(r'^run_check(?:_tri)?\s+"([^"]+)"', board.read_text(encoding="utf-8") if board.is_file() else "", re.M))
    print("PLAN contracts: " + (", ".join(sorted(cc.affected_rows(root, paths) & per_merge)) or "nightly"))
    print("PLAN tools_tests: " + (", ".join(rtt.scoped_modules(rtt.discover(), paths)) or "nightly"))
    print("PLAN scripts_tests: " + (", ".join(rtt.scoped_modules(rst.select_modules(rst.discover(), rst.load_ownership()), paths)) or "nightly"))
    print("PLAN laptop sim: " + (", ".join(merge_gate.laptop_sim_rows(paths)) or "nightly"))
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
    preview = (lane_merge_conflicts.ConflictPreview(root, args.conflict_dir, "HEAD", lambda f: is_generated(f) or is_regenerable(f))
               if getattr(args, "conflict_dir", None) else None)
    for i, ref in enumerate(refs, 1):
        p = plan(root, ref, head_ref)
        if preview and not p["contained"]:
            ptree, pconf = virtual_merge(root, p["sha"], head_ref)
            preview.add_tip(i, ref, p["sha"], ptree, pconf)
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
    if preview:
        print(preview.one_line(len(refs)))
    return would_abort


def merge_tip(root: str, tools_dir: str, p: dict, message: str, resolutions: dict, *, batch=False) -> tuple[str, str | None, list]:
    """Merge one planned tip, regenerate the ratchets, commit the tightened baselines. -> (merge sha, follow-up sha, failed ratchets)."""
    require_clean_generator_inputs(root)
    pre_dirty = {path for _, path in status_entries(root)}
    do_merge(root, p["sha"], message, lane_files=p["lane_files"], resolutions=resolutions)
    merge_sha = git_out(root, "rev-parse", "--short=9", "HEAD")
    import contracts_cache as cc
    affected = cc.affected_rows(root, p["lane_files"])
    ratchets = [r for r in RATCHETS if ("cxx17_ratchet" if r[0] == "cxx17" else r[0]) in affected]
    if batch:
        return merge_sha, None, []  # one merged-tree ratchet pass at batch end
    semantic = semantic_precheck(root, tools_dir, p["lane_files"]) or lane_merge_baseline.base_baseline_check(root, tools_dir, git_out(root, "rev-parse", "HEAD^1"), ratchets, RATCHET_BASELINES, run, git, lines)
    if semantic:
        return merge_sha, None, semantic
    artifacts = regenerate_all(root, base=p["head"])
    artifact_follow = None
    if artifacts:
        try:
            artifact_follow = _git_util.commit_paths(root, artifacts, ARTIFACT_SUBJECT, TRAILER or None)
            print(f"lane_merge: {ARTIFACT_SUBJECT}: {artifact_follow}")
        except _git_util.GitError as exc:
            raise Stop(6, "artifact follow-up commit failed: " + str(exc)) from exc
    results = run_ratchets(root, tools_dir, rows=ratchets)
    follow = commit_tightened(root, pre_dirty)
    return merge_sha, follow or artifact_follow, [(label, rc, tail) for label, rc, tail in results if rc != 0]


def cpu_mode(args) -> str:
    """"gate" (CPUs 0-7) only for the merge itself (--batch or a single tip); --lint, --precheck, --pre-gate, --regenerate-all are lane work."""
    lane_only = args.lint or args.precheck or args.pre_gate or args.regenerate_all
    return "gate" if (args.batch or args.ref) and not lane_only else "lane"


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    ap = build_parser()
    args = ap.parse_args(argv)
    pin_cpus(cpu_mode(args))   # children inherit; only --batch / merge-gate stages take the gate half (CPU_SPLIT), lane modes stay off it
    if (rc := lane_merge_trust.delegate(args, list(sys.argv[1:] if argv is None else argv))) is not None:
        return rc
    if args.lint:
        return lint(args.root, args.lint, **({"base": args.base or "HEAD"} if args.base or args.batch else {}))
    if args.precheck:
        return precheck(args.root, args.precheck, True if args.with_link else False if args.no_link else None,
                        not args.no_sim_rows, args.sim_install,
                        **({"base": args.base or "HEAD"} if args.base or args.batch else {}))
    if args.pre_gate:
        return pre_gate(args.root, args.exe)
    try:
        if args.promote:
            return promote(args.root, args.promote, args.verdict)
        if args.bisect:
            if args.red == args.green:
                ap.error("--bisect needs exactly one of --red / --green")
            return bisect(args.root, args.bisect, args.green)
        if args.stage:
            if not args.batch:
                ap.error("--stage needs --batch TIP ...")
            if not args.dry_run:
                stage(args.root, args.stage, args.batch)
    except Stop as e:
        print(f"lane_merge: {e}", file=sys.stderr)
        return e.code
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
            if args.conflict_dir and not batch:
                ap.error("--conflict-dir needs --batch")
            would_abort = plan_batch(root, args, refs, resolutions_for)
            print("PLAN then: path-triggered ratchets"
                  + "; regenerate-all and commit changed artifacts; follow-up commit of tightened baselines only"
                  + (" (check each tip; tighten/regenerate/commit/verify once at the end)" if batch else ""))
            print("[dry-run] nothing changed")
            if would_abort:
                print("lane_merge: the merge would abort on the blocking conflicts above", file=sys.stderr)
            return 5 if would_abort and not args.conflict_dir else 0   # with --conflict-dir the folder is the report
        refuse_direct_nifty(root, args.hotfix)
        if args.conflict_dir:
            ap.error("--conflict-dir only works with --dry-run")
        if batch:
            missing = missing_receipts(root, refs)
            print(f"precheck receipts: {len(refs) - len(missing)}/{len(refs)}")
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
            tag =f"MERGED {merge_sha}" + (f" (+{follow})" if follow else "") + f" files={len(p['lane_files'])}"
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
        if batch and merged:
            import contracts_cache as cc
            paths = lines(git_out(root, "diff", "--name-only", window_base, "HEAD"))
            affected = cc.affected_rows(root, paths)
            ratchets = [r for r in RATCHETS if ("cxx17_ratchet" if r[0] == "cxx17" else r[0]) in affected]
            semantic = semantic_precheck(root, tools_dir, paths)
            accepted = lane_merge_baseline.base_baseline_check(root, tools_dir, window_base, ratchets, RATCHET_BASELINES, run, git, lines)
            checked = run_ratchets(root, tools_dir, tighten=False, rows=ratchets)
            debt = semantic + accepted + [(label, rc, tail) for label, rc, tail in checked if rc]
            report_ratchet_debt(debt)
            pre_dirty = {path for _, path in status_entries(root)}
            artifacts = regenerate_derived(root, window_base)
            record_ratchet_baselines(root, tools_dir, ratchets)
            artifacts += ["docs/split-map.md"] if any(r[0] == "split_map" for r in ratchets) else []
            rechecked = run_ratchets(root, tools_dir, tighten=False, rows=ratchets)
            remaining = [(label, rc, tail) for label, rc, tail in rechecked if rc]
            remaining += semantic_precheck(root, tools_dir, paths)
            if remaining:
                raise Stop(6, "ratchet recheck after regeneration failed: " + str(remaining))
            follow = commit_tightened(root, pre_dirty, artifacts)
            merge_sha, _, count = merged[-1]
            merged[-1] = (merge_sha, follow, count)
        if not merged and not batch:
            return 0
        mode = "none" if args.no_verify else args.verify
        record_path = Path(root) / "build64-ninja/merge-window.json"
        derived_notes = (json.loads(record_path.read_text(encoding="utf-8")).get("derived_notes", [])
                         if record_path.exists() else [])
        record = dict(derived_notes=derived_notes, base=window_base, head=git_out(root, "rev-parse", "HEAD"), started=window_started,
                      tree=git_out(root, "rev-parse", "HEAD^{tree}"), **({"hotfix": True} if args.hotfix else {}))
        record_path = Path(root) / "build64-ninja/merge-window.json"
        if batch and merged and not stopped:
            qfailed = quick_rows(root, base=window_base)
            if qfailed:
                for label, rc, tail in qfailed:
                    print(f"lane_merge: quick row {label} exited {rc}; last lines:\n{tail}", file=sys.stderr)
                print(f"BATCH STOPPED at quick rows ({', '.join(q[0] for q in qfailed)}) merged={len(merged)} remaining=(none)")
                return 6
            commit_tightened(root, set(), GENERATED)
        if batch and merged and not stopped and args.stage and not batch_lint_ok(root):
            print(f"BATCH STOPPED at lint (merged={len(merged)}); fix, commit, re-run --lint HEAD"); return 6
        record.update(head=git_out(root, "rev-parse", "HEAD"), tree=git_out(root, "rev-parse", "HEAD^{tree}"))
        if stopped:
            verdict, vresults = "SKIPPED(ratchet-failure)", []
        else:
            verdict, vresults = verify_merged(root, mode)
            if batch and verdict == "OK" and mode != "none":   # --no-verify staging builds nothing on the PC (laptop_gate builds)
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
