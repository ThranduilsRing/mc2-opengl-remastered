#!/usr/bin/env python3
"""run_tidy - clang-tidy over the compile DB on the Linux box, through LLVM's run-clang-tidy + ctcache.

  full:  py -3 tools/box_run.py --gpu auto -- 'sh:python3 tools/tidy/run_tidy.py'
  diff:  ... 'sh:python3 tools/tidy/run_tidy.py --diff <base>'     only findings on lines changed since <base> (no baseline file)
  fix:   ... 'sh:python3 tools/tidy/run_tidy.py --fix --dir mclib'  safe-set fix-its on clean files (see Guards)

The work is done by LLVM's run-clang-tidy / clang-tidy-diff.py with -clang-tidy-binary = the ctcache wrapper (unchanged TUs are answered
from the local disk cache, /workspace/cache/ctcache). This file only chooses the TUs, applies the guards and pins the cores.

Guards (kept from tools/tidy/run_tidy_safe.py, which holds the lists): the EXCLUDED globs (3rdparty, build trees, Gates, files other
lanes are refactoring) and the --exclude-lane files are never tidied; --fix also refuses files with uncommitted changes, files absent
from compile_commands.json, anything outside the repo, headers, and any check outside SAFE_CHECKS. --fix bypasses the cache and runs
header-filter ^$ (main file only), as run_tidy_safe did. The object-identity proof of a --fix batch is NOT done here (it needs the MSVC
objects): follow a --fix batch with the box build plus smoke-lite, or run run_tidy_safe's proof on the PC.

Exit: 0 clean (full runs are report-only, as .clang-tidy says), 1 findings in --diff mode, 2 refused, 3 INFRA (tool or DB missing, not
Linux). It never reports a pass it did not run.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

CTCACHE = os.environ.get("MC2_CTCACHE", "/workspace/tools/ctcache/clang-tidy")
CACHE_DIR = os.environ.get("MC2_CTCACHE_DIR", "/workspace/cache/ctcache")
BUILD_CORES = "32-63"
MAX_JOBS = 32
SRC_EXT = (".cpp", ".cc", ".cxx")
HDR_EXT = SRC_EXT + (".h", ".hpp", ".hh", ".inl")
FINDING = re.compile(r"^(\S.*?):(\d+):(\d+): (warning|error): ", re.M)


def infra(msg: str) -> int:
    print(f"run_tidy: INFRA: {msg}", file=sys.stderr)
    return 3


def find_tool(names, extra=()):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    for p in extra:
        if os.path.exists(p):
            return p
    return None


def load_db(tree: str) -> list:
    with open(os.path.join(tree, "compile_commands.json"), encoding="utf-8") as fh:
        db = json.load(fh)
    out = []
    for ent in db:
        f = ent["file"] if os.path.isabs(ent["file"]) else os.path.join(ent["directory"], ent["file"])
        out.append(os.path.normpath(f))
    return out


def select(guards, files, dirs, db_abs: list, fix: bool):
    """-> (absolute TU paths to tidy, [(rel, reason)] skipped). Same guard order as run_tidy_safe.select."""
    want = None
    if files or dirs:
        want = {os.path.normpath(os.path.abspath(f)) for f in files}
    in_db = set(db_abs)
    dirty = guards.dirty_files() if fix else set()
    kept, skipped = [], []
    for p in sorted(in_db):
        rel = os.path.relpath(p, ROOT).replace("\\", "/")
        if want is not None and p not in want and not any(rel.startswith(d.rstrip("/") + "/") for d in dirs):
            continue
        if rel.startswith(".."):
            skipped.append((rel, "outside the repo"))
        elif guards.is_excluded(rel):
            skipped.append((rel, "excluded: third party, gate module or another lane's file"))
        elif rel in dirty:
            skipped.append((rel, "has uncommitted changes"))
        else:
            kept.append(p)
    if want:
        for p in sorted(want - in_db):
            rel = os.path.relpath(p, ROOT).replace("\\", "/")
            skipped.append((rel, "headers are not TUs (use --diff)" if p.lower().endswith(HDR_EXT[3:]) else "not in compile_commands.json"))
    return kept, skipped


def tool_env(use_cache: bool) -> dict:
    env = dict(os.environ)
    if use_cache:
        tidy = find_tool(["clang-tidy-20", "clang-tidy"], ["/usr/lib/llvm-20/bin/clang-tidy"])
        env.update(CTCACHE_CLANG_TIDY=tidy, CTCACHE_DIR=CACHE_DIR, CTCACHE_LOCAL="1",
                   # a hit must not depend on which worktree/lane root the TU was preprocessed in
                   CTCACHE_STRIP=os.pathsep.join(re.escape(p) for p in {ROOT, os.path.realpath(ROOT)}))
        os.makedirs(CACHE_DIR, exist_ok=True)
    return env


def pinned(cmd: list) -> list:
    ts = shutil.which("taskset")
    return ([ts, "-c", BUILD_CORES] if ts and (os.cpu_count() or 0) >= 64 else []) + cmd


def run_full(args, tree, tus, jobs, run_ct, tidy_bin):
    cmd = [run_ct, "-p", tree, "-clang-tidy-binary", tidy_bin, "-j", str(jobs), "-quiet"]
    if args.checks:
        cmd += ["-checks", args.checks]
    if args.fix:
        cmd += ["-fix", "-header-filter", "^$", "-checks", "-*," + args.checks, "-clang-apply-replacements-binary",
                find_tool(["clang-apply-replacements-20", "clang-apply-replacements"]) or "clang-apply-replacements"]
    cmd += ["^" + re.escape(p) + "$" for p in tus]
    r = subprocess.run(pinned(cmd), cwd=ROOT, env=tool_env(not args.fix and not args.no_cache), capture_output=True, text=True, errors="replace",
                       **console_child.child_kwargs(redirected=True))
    return r


def run_diff(args, tree, jobs, diff_py, tidy_bin, guards):
    d = subprocess.run(["git", "-C", ROOT, "diff", "-U0", "--no-color", args.diff, "--"] + [f"*{e}" for e in HDR_EXT],
                       capture_output=True, text=True, errors="replace", **console_child.child_kwargs(redirected=True))
    if d.returncode:
        return None, d.stderr
    # drop the diff hunks of excluded files so the guards hold here as well
    parts = re.split(r"(?m)^(?=diff --git )", d.stdout)
    keep = [p for p in parts if p and not guards.is_excluded((re.match(r"diff --git a/(\S+)", p) or [0, ""])[1])]
    if not any(p.startswith("diff --git ") for p in keep):
        return subprocess.CompletedProcess([], 0, "", ""), 0
    cmd = [sys.executable, diff_py, "-p1", "-path", tree, "-j", str(jobs), "-quiet", "-clang-tidy-binary", tidy_bin]
    r = subprocess.run(pinned(cmd), cwd=ROOT, env=tool_env(not args.no_cache), input="".join(keep), capture_output=True, text=True,
                       errors="replace", **console_child.child_kwargs(redirected=True))
    return r, len(keep)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*", help="TUs to tidy (default: every guarded TU of the compile DB)")
    ap.add_argument("--dir", action="append", default=[], help="tidy every TU under this repo-relative dir")
    ap.add_argument("--tree", default="build64-ninja", help="build dir with compile_commands.json")
    ap.add_argument("--diff", metavar="BASE", help="only findings on lines changed since BASE (clang-tidy-diff)")
    ap.add_argument("--fix", action="store_true", help="apply SAFE_CHECKS fix-its (clean, guarded, uncached)")
    ap.add_argument("--checks", help="run these checks instead of .clang-tidy's (with --fix: must be in SAFE_CHECKS)")
    ap.add_argument("-j", "--jobs", type=int, default=MAX_JOBS)
    ap.add_argument("--no-cache", action="store_true", help="bypass ctcache")
    ap.add_argument("--list-excluded", action="store_true")
    import run_tidy_safe as guards  # the guard lists live there until it retires
    guards.add_lane_args(ap)
    args = ap.parse_args(argv)
    guards.apply_lane_args(args)
    if args.list_excluded:
        print("\n".join(guards.EXCLUDED) + ("\n" + "\n".join(sorted(guards.LANE_FILES)) if guards.LANE_FILES else ""))
        return 0
    if args.fix and args.diff:
        ap.error("--fix and --diff are separate modes")
    if args.fix:
        checks = tuple(c for c in (args.checks.split(",") if args.checks else guards.SAFE_CHECKS) if c)
        bad = [c for c in checks if c not in guards.SAFE_CHECKS]
        if bad:
            print("run_tidy: not in the safe set: " + ", ".join(bad), file=sys.stderr)
            return 2
        if not args.files and not args.dir:
            ap.error("--fix needs files or --dir (never repo-wide)")
        args.checks = ",".join(checks)
    if not sys.platform.startswith("linux"):
        return infra("runs on the Linux box only: py -3 tools/box_run.py --gpu auto -- 'sh:python3 tools/tidy/run_tidy.py ...'")
    tree = args.tree if os.path.isabs(args.tree) else os.path.join(ROOT, args.tree)
    if not os.path.exists(os.path.join(tree, "compile_commands.json")):
        return infra(f"{tree}/compile_commands.json missing (configure the box tree first)")
    run_ct = find_tool(["run-clang-tidy-20", "run-clang-tidy"], ["/usr/lib/llvm-20/bin/run-clang-tidy"])
    diff_py = find_tool(["clang-tidy-diff-20.py", "clang-tidy-diff.py"], ["/usr/lib/llvm-20/share/clang/clang-tidy-diff.py"])
    tidy_bin = CTCACHE if not (args.fix or args.no_cache) else find_tool(["clang-tidy-20", "clang-tidy"])
    if not (run_ct and diff_py and tidy_bin):
        return infra("run-clang-tidy / clang-tidy-diff.py / clang-tidy not found")
    if tidy_bin == CTCACHE and not os.path.exists(CTCACHE):
        return infra(f"ctcache missing at {CTCACHE} (see A:/Games/mc2-ops/box-game-runs.md Tools)")
    jobs = max(1, min(args.jobs, MAX_JOBS))

    import time
    t0 = time.time()
    if args.diff:
        r, n = run_diff(args, tree, jobs, diff_py, tidy_bin, guards)
        if r is None:
            return infra("git diff failed: " + n.strip()[:200])
        mode = f"diff vs {args.diff}, {n} changed file(s)"
    else:
        tus, skipped = select(guards, args.files, args.dir, load_db(tree), args.fix)
        for rel, why in skipped:
            print(f"SKIP  {rel}  ({why})")
        if not tus:
            print("run_tidy: nothing to do")
            return 0
        r = run_full(args, tree, tus, jobs, run_ct, tidy_bin)
        mode = f"{'fix' if args.fix else 'full'}, {len(tus)} TU(s)"
    out = (r.stdout or "") + (r.stderr or "")
    findings = sorted({m.group(0).strip() for m in FINDING.finditer(out)})
    sys.stdout.write(out if args.diff or args.fix else "\n".join(l for l in out.splitlines() if FINDING.match(l)) + "\n")
    print(f"run_tidy: {mode}, -j{jobs}, {len(findings)} finding line(s), rc={r.returncode}, {time.time() - t0:.1f}s")
    if r.returncode and not findings:
        return infra(f"run-clang-tidy exited {r.returncode} with no findings (tool failure, not a pass)")
    if args.fix:
        print("run_tidy: --fix batch applied; prove it with the box build + smoke-lite (no object proof here); commit by pathspec")
    return 1 if (args.diff and findings) else 0


if __name__ == "__main__":
    sys.exit(main())
