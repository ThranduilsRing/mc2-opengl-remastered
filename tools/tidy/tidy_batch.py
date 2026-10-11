#!/usr/bin/env python3
"""tidy_batch - run N tidy batches end to end: pick, rewrite, prove, verify, commit.

WHAT: for each batch it (1) picks the next files that still have work (pick_batch), (2) records the stock mc2.fx dump/rebuild of the
current tree, (3) runs run_tidy_safe on them (clang-tidy safe set, or --rewrite NAME), whose objhash proof keeps only identical objects,
(4) verifies the root all-targets build, the mc2fx dump/rebuild (byte-identical to step 2) and the doctest tier, (5) commits the kept files by
pathspec and then the tightened cxx17 ratchet baseline. A failed verification stops the run with the worktree left dirty for a human.
WHY: this was a pile of shell scripts in a temp directory; the sequence and its stop conditions are the part worth keeping.

  py -3 tools/tidy/tidy_batch.py --dir mclib --dir code --count 50 --batches 3 --first-number 18 --rewrite null-tokens --done tidy_done.txt
  py -3 tools/tidy/tidy_batch.py --dir mclib --count 50 --dry-run
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pick_batch  # noqa: E402
import run_tidy_safe as rts  # noqa: E402

ROOT = rts.ROOT
console_child = rts.console_child
_git_util = rts._git_util
TRAILER = "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
FX_TOOL = "out/tools/mc2fx/mc2fx.exe"
STOCK_FX = "A:/Games/mc2-opengl/mc2-win64-v0.4/data/effects/mc2.fx"

_SUMMARY = re.compile(r"run_tidy_safe: (\d+) proven (?:identical|compiling), (\d+) reverted, (\d+) skipped")
_TOTAL = re.compile(r"^TOTAL\s+(\d+)\s+(\d+)", re.M)


def parse_summary(text: str):
    """-> (proven, reverted, skipped) from run_tidy_safe's last summary line, or None."""
    m = None
    for m in _SUMMARY.finditer(text):
        pass
    return tuple(int(x) for x in m.groups()) if m else None


def parse_totals(text: str):
    """-> (LG01, LG02) from the TOTAL row of the cxx17 ratchet report, or None."""
    m = _TOTAL.search(text)
    return (int(m.group(1)), int(m.group(2))) if m else None


def commit_message(number: int, summary, totals, rewrite) -> str:
    proven, reverted, skipped = summary
    how = f"run_tidy_safe --rewrite {rewrite}" if rewrite else "run_tidy_safe (clang-tidy safe set)"
    lg = f" LG01 {totals[0]}, LG02 {totals[1]}." if totals else ""
    return (f"tidy: batch {number} - {proven} TUs ({reverted} reverted, {skipped} skipped) through {how}\n\n"
            f"Each rebuilt object compared with tools/objhash.py; mc2fx dump/rebuild byte-identical; doctest tier green.{lg}")


def run(argv, cwd=ROOT):
    return subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                          **console_child.child_kwargs(redirected=True))


def fx_roundtrip(tree: str, out_dir: str) -> bool:
    """mc2fx dump + rebuild of the stock effects file into out_dir; False when the tool or the file is missing or fails."""
    exe = os.path.join(ROOT, tree, FX_TOOL)
    if not (os.path.exists(exe) and os.path.exists(STOCK_FX)):
        return False
    os.makedirs(out_dir, exist_ok=True)
    a = run([exe, "dump", "--full", STOCK_FX, os.path.join(out_dir, "dump_full.json")])
    b = run([exe, "rebuild", STOCK_FX, os.path.join(out_dir, "rebuilt.fx")])
    return a.returncode == 0 and b.returncode == 0


def same_files(a: str, b: str, names=("dump_full.json", "rebuilt.fx")) -> bool:
    for n in names:
        pa, pb = os.path.join(a, n), os.path.join(b, n)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            return False
        with open(pa, "rb") as fa, open(pb, "rb") as fb:
            if fa.read() != fb.read():
                return False
    return True


def verify(tree: str, base_fx: str, now_fx: str, runner=run):
    """-> (ok, message). Root all-targets build, mc2fx identical, doctest tier."""
    b = runner([sys.executable, os.path.join(ROOT, "tools", "build_all_targets.py"), "--root-only", "--keep-going", "-j6"])
    if b.returncode:
        return False, "root build failed:\n" + "\n".join(b.stdout.splitlines()[-8:])
    if not fx_roundtrip(tree, now_fx) or not same_files(base_fx, now_fx):
        return False, "mc2fx dump/rebuild differs from (or could not be compared with) the pre-batch result"
    t = runner([sys.executable, os.path.join(ROOT, "tools", "build_contract_harnesses.py"), "--run", "--unit"])
    if t.returncode:
        return False, "doctest tier failed:\n" + "\n".join(t.stdout.splitlines()[-8:])
    return True, "ok"


def one_batch(args, number: int, runner=run):
    """-> (status, detail): 'empty', 'failed' or 'committed'."""
    files = pick_batch.pick(args.dir, args.count, args.tree, args.done_set, False, args.rewrite)
    if not files:
        return "empty", "no candidate files"
    if args.done:
        with open(args.done, "a", encoding="utf-8", newline="\n") as fh:
            fh.writelines(f + "\n" for f in files)
        args.done_set.update(files)
    print(f"batch {number}: picked {len(files)}", flush=True)
    if args.dry_run:
        print("\n".join(files))
        return "empty", "dry run"
    tmp = tempfile.mkdtemp(prefix="tidy_batch_")
    base_fx, now_fx = os.path.join(tmp, "base"), os.path.join(tmp, "now")
    try:
        fx_roundtrip(args.tree, base_fx)
        cmd = [sys.executable, os.path.join(HERE, "run_tidy_safe.py"), "--tree", args.tree, "--jobs", "4", "--build-jobs", "4"]
        if args.rewrite:
            cmd += ["--rewrite", args.rewrite]
        r = runner(cmd + files)
        summary = parse_summary(r.stdout)
        if r.returncode not in (0, 1) or not summary:
            return "failed", "run_tidy_safe did not finish:\n" + "\n".join(r.stdout.splitlines()[-8:])
        print(f"batch {number}: {summary[0]} proven identical, {summary[1]} reverted, {summary[2]} skipped", flush=True)
        ok, msg = verify(args.tree, base_fx, now_fx, runner)
        if not ok:
            return "failed", msg
        kept = [p for p in _git_util.dirty_paths(ROOT, tracked_only=True) if not p.startswith("tools/")]
        if not kept:
            return "empty", "nothing changed"
        totals = parse_totals(runner([sys.executable, os.path.join(ROOT, "tools", "cxx17_ratchet", "cxx17_ratchet.py"),
                                      "--report", "--worktree"]).stdout)
        sha = _git_util.commit_paths(ROOT, kept, commit_message(number, summary, totals, args.rewrite), TRAILER)
        runner([sys.executable, os.path.join(ROOT, "tools", "cxx17_ratchet", "cxx17_ratchet.py"), "--check", "--tighten"])
        if "tools/cxx17_ratchet/baseline.json" in _git_util.dirty_paths(ROOT, tracked_only=True):
            sha += " + " + _git_util.commit_paths(ROOT, ["tools/cxx17_ratchet/baseline.json"],
                                                  f"cxx17 ratchet baseline tightened after tidy batch {number}", TRAILER)
        return "committed", f"{sha}; LG01/LG02 {totals}"
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", action="append", required=True)
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--batches", type=int, default=1)
    ap.add_argument("--first-number", type=int, default=1)
    ap.add_argument("--tree", default="build-alltargets")
    ap.add_argument("--rewrite", choices=rts.REWRITERS)
    ap.add_argument("--done", help="ledger of files already tried (skipped; picked ones are appended)")
    ap.add_argument("--dry-run", action="store_true", help="list the first batch and stop")
    args = ap.parse_args(argv)
    args.done_set = set()
    if args.done and os.path.exists(args.done):
        with open(args.done, encoding="utf-8") as fh:
            args.done_set = {ln.strip() for ln in fh if ln.strip()}
    rts.apply_lane_args(argparse.Namespace(exclude_lane=None))      # the default lane-2 exclusion
    for i in range(args.batches):
        status, detail = one_batch(args, args.first_number + i)
        print(f"batch {args.first_number + i}: {status} - {detail}", flush=True)
        if status != "committed":
            return 0 if status == "empty" else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
