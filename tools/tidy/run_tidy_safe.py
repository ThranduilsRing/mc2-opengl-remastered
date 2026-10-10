#!/usr/bin/env python3
"""run_tidy_safe - apply the clang-tidy "safe set" to translation units and PROVE the object code did not change.

WHAT: for each .cpp, run only checks whose rewrite cannot change behaviour (NULL -> nullptr, typedef -> using,
add `override`, drop a cast to the expression's own type), then rebuild that TU's object and compare its COFF
behaviour sections with the pre-edit object (tools/objhash.py). A TU whose object differs is reverted.
WHY: the C++23 program (.claude/cleanup/cxx23-step1-tidy-plan.md) wants thousands of mechanical rewrites; a
reviewer cannot read them, but identical object sections prove each one was a no-op.

  py -3 tools/tidy/run_tidy_safe.py --list-excluded
  py -3 tools/tidy/run_tidy_safe.py --dry-run mclib/crater.cpp          # tidy, show the diff, change nothing kept
  py -3 tools/tidy/run_tidy_safe.py mclib/crater.cpp mclib/dir.cpp      # keep proven rewrites in the worktree
  py -3 tools/tidy/run_tidy_safe.py --dir mclib --limit 25 --jobs 4

Phases (the object proof needs a quiet, serial build, so only the tidy parse is parallel):
  A. hash   the current objects (they must be newer than their sources: a clean base build);
  B. tidy   each TU with --fix, diagnostics and fixes restricted to the main file (header-filter ^$);
  C. build  all changed objects in ONE ninja invocation under the MSVC environment;
  D. prove  compare each object; keep identical ones, `git checkout` the rest and report them.
Nothing is committed; the report ends with the pathspec list of kept files and the ratchet command to run.

Refused up front: the owner's comment-pass files (EXCLUDED), files with uncommitted changes (a fix-it would mix
with someone's work), files absent from compile_commands.json, and anything outside the repo.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402
import objhash  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "tools"))
import _git_util  # noqa: E402  (shared git helpers)

#: Checks whose rewrite is a behavioural no-op. Anything that can reorder initialisation, change overload
#: resolution of a call, or alter evaluation (modernize-use-default-member-init, modernize-loop-convert,
#: modernize-use-auto) stays off until it has its own proof plan.
SAFE_CHECKS = (
    "modernize-use-nullptr",
    "modernize-use-using",
    "modernize-use-override",
    "readability-redundant-casting",
)

#: Never tidied: third-party code, the gate module, and the files lane 2 is refactoring. (The owner's comment-only pass finished
#: 2026-10-11, so its files are no longer listed.)
EXCLUDED = (
    "3rdparty/*", "build*/*", "Gates/*",
    # Lane 2 is refactoring these (phase helpers / splits); a fix-it would collide with its merge.
    "*/gameosmain.cpp", "*/saveload.cpp", "*/gos_static_prop_registry.cpp", "*/tgl_transform.cpp",
    "*/mission_update.cpp", "*/missionbegin.cpp", "*/gos_terrain_indirect_dispatch.cpp", "*/gamecam.cpp",
    "*/EditorInspector.cpp", "*/mlrlightmap.cpp", "*/brain_special_dispatch*", "*clip*",
)

DEFAULT_TREE = "build64-ninja"


#: The other builder lane's branch: every file it changed since it forked is off limits to a tidy batch, so the two lanes never edit
#: the same file (tools/lane_merge.py refuses a dirty overlap).
DEFAULT_LANE = "claude/lane2-s3"
LANE_FILES: set = set()


def lane_changed_files(branch: str, root: str = ROOT) -> set:
    """Repo-relative files `branch` changed since it diverged from HEAD (empty once it is fully merged)."""
    base = _git_util.git_out(root, "merge-base", "HEAD", branch)
    out = _git_util.git(root, "diff", "--name-only", "-z", base, branch).stdout
    return {p for p in out.split("\0") if p}


def add_lane_args(ap) -> None:
    ap.add_argument("--exclude-lane", action="append", default=None, metavar="BRANCH",
                    help=f"exclude every file this branch changed since the fork (repeatable; default {DEFAULT_LANE} when it exists; "
                         "pass an empty string to disable)")


def apply_lane_args(args) -> None:
    """Load LANE_FILES from --exclude-lane; sets args.lane_excluded_note for the run report."""
    explicit = args.exclude_lane is not None
    branches = [b for b in (args.exclude_lane if explicit else [DEFAULT_LANE]) if b]
    LANE_FILES.clear()
    args.lane_excluded_note = ""
    for b in branches:
        if _git_util.git(ROOT, "rev-parse", "--verify", "--quiet", b).returncode != 0:
            if explicit:
                sys.exit(f"run_tidy_safe: --exclude-lane {b}: no such branch")
            continue
        LANE_FILES.update(lane_changed_files(b))
        args.lane_excluded_note += f"excluding {len(LANE_FILES)} file(s) changed on {b}\n"
    args.lane_excluded_note = args.lane_excluded_note.strip()


def is_excluded(rel: str) -> bool:
    return rel in LANE_FILES or any(fnmatch.fnmatch(rel, pat) for pat in EXCLUDED)


def clang_tidy_exe() -> str:
    found = shutil.which("clang-tidy")
    if found:
        return found
    fallback = "C:/Program Files/LLVM/bin/clang-tidy.exe"
    if os.path.exists(fallback):
        return fallback
    sys.exit("run_tidy_safe: clang-tidy not found (install LLVM or put it on PATH)")


def tidy_command(exe: str, tree: str, rel: str, checks=SAFE_CHECKS, fix: bool = True) -> list:
    cmd = [exe, "-p", tree, "--checks=-*," + ",".join(checks), "--header-filter=^$", "--quiet"]
    if fix:
        cmd.append("--fix")
    return cmd + [rel]


def load_compile_db(tree: str) -> set:
    path = os.path.join(tree, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit(f"run_tidy_safe: {path} missing; run scripts/configure_ninja.py (it exports compile_commands.json)")
    with open(path, "r", encoding="utf-8") as fh:
        db = json.load(fh)
    out = set()
    for ent in db:
        f = os.path.normpath(ent["file"] if os.path.isabs(ent["file"]) else os.path.join(ent["directory"], ent["file"]))
        out.add(os.path.relpath(f, ROOT).replace("\\", "/"))
    return out


def git(*args) -> str:
    return _git_util.git(ROOT, *args).stdout


def dirty_files() -> set:
    return _git_util.dirty_paths(ROOT)


def select(paths, in_db: set, dirty: set):
    """-> (kept[rel], skipped[(rel, reason)])."""
    kept, skipped = [], []
    for p in paths:
        rel = os.path.relpath(os.path.abspath(p), ROOT).replace("\\", "/")
        if rel.startswith(".."):
            skipped.append((rel, "outside the repo"))
        elif not rel.lower().endswith((".cpp", ".cc", ".cxx")):
            skipped.append((rel, "not a translation unit (headers are a later phase)"))
        elif is_excluded(rel):
            skipped.append((rel, "excluded: owned by the comment pass or third party"))
        elif rel in dirty:
            skipped.append((rel, "has uncommitted changes"))
        elif rel not in in_db:
            skipped.append((rel, "not in compile_commands.json"))
        else:
            kept.append(rel)
    return kept, skipped


def restore_file(rel: str, mtime_ns: int) -> None:
    """Discard a file's working-tree edits and put its mtime back, so a restored source does not look newer than
    the object that was built from it (which would make the next proof refuse the base build)."""
    subprocess.run(["git", "-C", ROOT, "checkout", "--", rel], **console_child.child_kwargs(redirected=True))
    os.utime(os.path.join(ROOT, rel), ns=(mtime_ns, mtime_ns))


def ninja_objects(tree: str, rels, jobs: int):
    """Build the objects of `rels` in one ninja run under the MSVC environment (keep going past failures).

    -> (returncode, [object targets that failed to compile])."""
    targets = []
    for rel in rels:
        for o in objhash.find_objs(tree, rel):
            targets.append(os.path.relpath(o, tree).replace("\\", "/"))
    return ninja_objects_targets(tree, targets, jobs)


def ninja_objects_targets(tree: str, targets, jobs: int):
    """ninja -k 0 over object targets given relative to the tree. -> (returncode, [targets that failed])."""
    import configure_ninja as cn
    if not targets:
        return 0, []
    targets = list(targets)
    rc_all, failed_all = 0, []
    # The Windows command line is limited (~8 KB): build in chunks of 25 objects. -k 0 keeps going past compile errors within a chunk.
    for i in range(0, len(targets), 25):
        inner = subprocess.list2cmdline([cn.NINJA, "-C", tree, f"-j{jobs}", "-k", "0"] + targets[i:i + 25])
        # Freeze the build-fingerprint header: the edit makes the tree dirty, which would otherwise change the dirty flag compiled
        # into every TU that embeds it (see cmake/GenerateBuildFingerprint.cmake).
        r = subprocess.run(f'call "{cn.VCVARS}" >nul && {inner}', shell=True, capture_output=True, text=True, errors="replace",
                           env={**os.environ, "MC2_FINGERPRINT_FREEZE": "1"},
                           **console_child.child_kwargs(redirected=True))
        out = (r.stdout or "") + (r.stderr or "")
        failed = re.findall(r"^FAILED: (\S+)", out, re.M)
        for ln in out.splitlines():
            if "error C" in ln or ln.startswith("FAILED:"):
                print("  " + ln[:240], file=sys.stderr)
        if r.returncode and not failed:
            print("  ninja: " + " | ".join(out.strip().splitlines()[-3:])[:400], file=sys.stderr)
        rc_all = rc_all or r.returncode
        failed_all += failed
    return rc_all, failed_all


def run(args, tidy=None, build=None) -> int:
    tree = os.path.abspath(os.path.join(ROOT, args.tree)) if not os.path.isabs(args.tree) else args.tree
    paths = list(args.files)
    if args.dir:
        for d in args.dir:
            for dp, _dn, fn in os.walk(os.path.join(ROOT, d)):
                paths += [os.path.join(dp, f) for f in sorted(fn) if f.endswith((".cpp", ".cc", ".cxx"))]
    kept, skipped = select(sorted(set(paths)), load_compile_db(tree), dirty_files())
    if args.limit:
        kept = kept[:args.limit]
    if getattr(args, "lane_excluded_note", ""):
        print(args.lane_excluded_note)
    for rel, why in skipped:
        print(f"SKIP  {rel}  ({why})")
    if not kept:
        print("run_tidy_safe: nothing to do")
        return 0

    before, problems = objhash.hash_files(ROOT, tree, kept, tool="run_tidy_safe")
    for pr in problems:
        print("run_tidy_safe: " + pr, file=sys.stderr)
    if problems:
        print("run_tidy_safe: refusing to edit without a clean base build of every selected TU", file=sys.stderr)
        return 2

    mtimes = {rel: os.stat(os.path.join(ROOT, rel)).st_mtime_ns for rel in kept}
    checks = tuple(c for c in (args.checks.split(",") if args.checks else SAFE_CHECKS) if c)
    unknown = [c for c in checks if c not in SAFE_CHECKS]
    if unknown:
        sys.exit("run_tidy_safe: not in the safe set: " + ", ".join(unknown))
    exe = clang_tidy_exe()
    tidy = tidy or (lambda cmd: subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                                               **console_child.child_kwargs(redirected=True)))
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda rel: (rel, tidy(tidy_command(exe, tree, rel, checks=checks, fix=True))), kept))

    changed, reverted = [], []
    for rel, res in results:
        if " error:" in (res.stdout or "") + (res.stderr or ""):
            restore_file(rel, mtimes[rel])
            reverted.append((rel, "clang-tidy reported a compile error; fix-its discarded"))
        elif git("diff", "--numstat", "--", rel).strip():
            changed.append(rel)
    for rel in changed:
        n_add, n_del, _ = git("diff", "--numstat", "--", rel).split("\t")
        print(f"EDIT  {rel}  +{n_add}/-{n_del}")
    if args.dry_run:
        for rel in changed:
            sys.stdout.write(git("diff", "--", rel))
            restore_file(rel, mtimes[rel])
        print(f"run_tidy_safe: dry run, {len(changed)} TU(s) would change; worktree restored")
        return 0
    if not changed:
        print("run_tidy_safe: no rewrites needed")
        return 0

    res = (build or ninja_objects)(tree, changed, args.build_jobs)
    rc, failed = res if isinstance(res, tuple) else (res, [])
    if rc:
        # A rewrite the compiler rejects (e.g. clang-tidy turning `typedef struct X {...} Y;` into an alias declaration with a
        # definition, which MSVC refuses) reverts that TU only; the rest are rebuilt and proved.
        failed_keys = {f.replace("\\", "/") for f in failed}
        bad = [rel for rel in changed
               if any(os.path.relpath(o, tree).replace("\\", "/") in failed_keys for o in objhash.find_objs(tree, rel))]
        if not bad:
            print(f"run_tidy_safe: ninja failed (rc={rc}) with no attributable object; reverting every edit of this batch",
                  file=sys.stderr)
            for rel in changed:
                restore_file(rel, mtimes[rel])
            return 1
        for rel in bad:
            restore_file(rel, mtimes[rel])
            reverted.append((rel, "does not compile after the rewrite"))
            changed.remove(rel)
        res = (build or ninja_objects)(tree, changed, args.build_jobs) if changed else (0, [])
        rc, failed = res if isinstance(res, tuple) else (res, [])
        if rc:
            print(f"run_tidy_safe: ninja failed again (rc={rc}); reverting every edit of this batch", file=sys.stderr)
            for rel in changed:
                restore_file(rel, mtimes[rel])
            return 1

    proven = []
    for rel in sorted(changed):
        lines, _bad = objhash.compare_files({**before, "files": {rel: before["files"][rel]}})
        if lines and all(ln.startswith("OK") for ln in lines):
            proven.append(rel)
        else:
            restore_file(rel, mtimes[rel])
            reverted.append((rel, "; ".join(lines)))
    for rel, why in reverted:
        print(f"REVERT {rel}  {why}")
    print(f"run_tidy_safe: {len(proven)} proven identical, {len(reverted)} reverted, {len(skipped)} skipped")
    if proven:
        print("kept (commit by pathspec): " + " ".join(proven))
        print("then: py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check --tighten")
    return 1 if reverted else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--dir", action="append", default=[], help="tidy every .cpp under this repo-relative dir")
    ap.add_argument("--tree", default=DEFAULT_TREE, help="Ninja build dir with compile_commands.json")
    ap.add_argument("--limit", type=int, default=0, help="at most N TUs this run (batch size)")
    ap.add_argument("--jobs", type=int, default=4, help="parallel clang-tidy parses")
    ap.add_argument("--build-jobs", type=int, default=4, help="ninja -j for the proof rebuild (keep low next to a smoke)")
    ap.add_argument("--dry-run", action="store_true", help="show the would-be diff, keep nothing")
    ap.add_argument("--checks", help="comma-separated clang-tidy checks to run instead of the whole safe set "
                                     "(each must be in SAFE_CHECKS), e.g. modernize-use-override")
    ap.add_argument("--list-excluded", action="store_true")
    add_lane_args(ap)
    args = ap.parse_args(argv)
    apply_lane_args(args)
    if args.list_excluded:
        print("\n".join(EXCLUDED) + ("\n" + "\n".join(sorted(LANE_FILES)) if LANE_FILES else ""))
        return 0
    if not args.files and not args.dir:
        ap.error("give files or --dir")
    import tidy_lock
    tree_abs = os.path.abspath(os.path.join(ROOT, args.tree)) if not os.path.isabs(args.tree) else args.tree
    try:
        with tidy_lock.TidyLock(tree_abs):
            return run(args)
    except tidy_lock.TidyBusy as e:
        print(f"run_tidy_safe: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
