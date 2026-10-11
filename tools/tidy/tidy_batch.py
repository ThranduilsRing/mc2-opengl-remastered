#!/usr/bin/env python3
"""tidy_batch - run N tidy batches end to end: pick, rewrite, prove, verify, commit.

WHAT: for each batch it (1) picks the next files that still have work (pick_batch), (2) records the stock mc2.fx dump/rebuild of the
current tree, (3) runs run_tidy_safe on them (clang-tidy safe set, or --rewrite NAME), whose objhash proof keeps only identical objects,
(4) verifies the root all-targets build, the mc2fx dump/rebuild (byte-identical to step 2) and the doctest tier, (5) commits the kept files by
pathspec and then the tightened cxx17 ratchet baseline. A failed verification stops the run with the worktree left dirty for a human.
WHY: this was a pile of shell scripts in a temp directory; the sequence and its stop conditions are the part worth keeping.

  py -3 tools/tidy/tidy_batch.py --dir mclib --dir code --count 50 --batches 3 --first-number 18 --rewrite null-tokens --done tidy_done.txt
  py -3 tools/tidy/tidy_batch.py --dir GameOS --count 50 --rewrite printf-buffers --corpus --log-identity --done pf_done.txt
  py -3 tools/tidy/tidy_batch.py --dir mclib --count 50 --dry-run
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pick_batch  # noqa: E402
import printf_corpus  # noqa: E402
import run_tidy_safe as rts  # noqa: E402

ROOT = rts.ROOT
sys.path.insert(0, os.path.join(ROOT, "tools"))
import log_identity  # noqa: E402  (game-log comparison by line shape)
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


def commit_message(number: int, summary, totals, rewrite, proof: str = "objhash", corpus: bool = False, logs: str = "") -> str:
    proven, reverted, skipped = summary
    how = f"run_tidy_safe --rewrite {rewrite}" if rewrite else "run_tidy_safe (clang-tidy safe set)"
    lg = f" LG01 {totals[0]}, LG02 {totals[1]}." if totals else ""
    if proof == "compile":
        what = ("Each file builds; the format corpus (tests/unit/printf_corpus.jsonl) passes in the doctest tier" if corpus
                else "Each file builds; doctest tier green") + "; log_tag_census unchanged." + (f" {logs}" if logs else "")
    else:
        what = "Each rebuilt object compared with tools/objhash.py; mc2fx dump/rebuild byte-identical; doctest tier green."
    return f"tidy: batch {number} - {proven} TUs ({reverted} reverted, {skipped} skipped) through {how}\n\n{what}{lg}"


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


def verify(tree: str, base_fx: str, now_fx: str, runner=run, census: bool = False, everything: bool = False):
    """-> (ok, message). All-targets build (root only unless `everything`: standalone harnesses and tools compile mclib/GameOS sources with
    their own include paths), mc2fx identical, doctest tier, and with `census` the log-tag census."""
    build = [sys.executable, os.path.join(ROOT, "tools", "build_all_targets.py"), "--keep-going", "-j6"]
    b = runner(build if everything else build + ["--root-only"])
    if b.returncode:
        return False, "build failed:\n" + "\n".join(b.stdout.splitlines()[-8:])
    if not fx_roundtrip(tree, now_fx) or not same_files(base_fx, now_fx):
        return False, "mc2fx dump/rebuild differs from (or could not be compared with) the pre-batch result"
    t = runner([sys.executable, os.path.join(ROOT, "tools", "build_contract_harnesses.py"), "--run", "--unit"])
    if t.returncode:
        return False, "doctest tier failed:\n" + "\n".join(t.stdout.splitlines()[-8:])
    if census:
        c = runner([sys.executable, os.path.join(ROOT, "tools", "log_tag_census.py"), "--check"])
        if c.returncode:
            return False, "log_tag_census differs from its baseline:\n" + c.stdout.strip()[-600:]
    return True, "ok"


SMOKE = ["--mission", "mc2_03", "--mission", "mc2_24", "--duration", "12", "--deploy", "0.4", "--keep-logs"]


def ensure_deployed(runner=run):
    """Build the dev tree from the committed HEAD and deploy it to the 0.4 install (the smoke fingerprint wants a committed exe). -> error text or None."""
    b = runner([sys.executable, os.path.join(ROOT, "scripts", "ninja_build.py")])
    if b.returncode:
        return "ninja_build failed:\n" + "\n".join(b.stdout.splitlines()[-6:])
    d = runner([sys.executable, os.path.join(ROOT, "scripts", "deploy_payload.py"), "--target", "game", "--ninja"])
    return None if d.returncode == 0 else "deploy_payload failed:\n" + "\n".join(d.stdout.splitlines()[-6:])


def capture_logs(dest: str, runner=run):
    """Run the two-mission smoke on the deployed exe and copy its artifact directory to `dest`. -> error text or None."""
    r = runner([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py")] + SMOKE)
    arts = sorted(glob.glob(os.path.join(ROOT, "tests", "smoke", "artifacts", "20*")), key=os.path.getmtime)
    if r.returncode or not arts:
        return "smoke failed (rc=%d):\n%s" % (r.returncode, "\n".join(r.stdout.splitlines()[-6:]))
    shutil.copytree(arts[-1], dest)
    return None


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
    dirty = [p for p in _git_util.dirty_paths(ROOT, tracked_only=True)]
    if dirty:
        return "failed", "the worktree has uncommitted tracked changes (a batch commits whatever is dirty): " + ", ".join(dirty[:5])
    compile_proof = args.proof == "compile" or (args.proof == "auto" and args.rewrite in rts.COMPILE_ONLY)
    tmp = tempfile.mkdtemp(prefix="tidy_batch_")
    base_fx, now_fx = os.path.join(tmp, "base"), os.path.join(tmp, "now")
    logs_before, logs_after = os.path.join(tmp, "logs_before"), os.path.join(tmp, "logs_after")
    try:
        if args.log_identity:
            err = ensure_deployed(runner) or capture_logs(logs_before, runner)
            if err:
                return "failed", "could not capture the before-logs: " + err
        fx_roundtrip(args.tree, base_fx)
        cmd = [sys.executable, os.path.join(HERE, "run_tidy_safe.py"), "--tree", args.tree, "--jobs", "4", "--build-jobs", "4"]
        if args.rewrite:
            cmd += ["--rewrite", args.rewrite]
        r = runner(cmd + files)
        summary = parse_summary(r.stdout)
        if r.returncode not in (0, 1) or not summary:
            return "failed", "run_tidy_safe did not finish:\n" + "\n".join(r.stdout.splitlines()[-8:])
        print(f"batch {number}: {summary[0]} proven {'compiling' if compile_proof else 'identical'}, {summary[1]} reverted, "
              f"{summary[2]} skipped", flush=True)
        changed = sorted(p for p in _git_util.dirty_paths(ROOT, tracked_only=True) if not p.startswith("tools/"))
        if not changed:
            return "empty", "nothing changed"
        if args.corpus:
            # the format pairs of the converted files, read from their pre-conversion text (HEAD), before the conversion is committed
            printf_corpus.add([p for p in changed if p.endswith((".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".inl"))], "HEAD")
            printf_corpus.write_generated()
        ok, msg = verify(args.tree, base_fx, now_fx, runner, census=compile_proof or args.corpus, everything=compile_proof)
        if not ok:
            return "failed", msg
        kept = [p for p in _git_util.dirty_paths(ROOT, tracked_only=True) if not p.startswith("tools/")]
        totals = parse_totals(runner([sys.executable, os.path.join(ROOT, "tools", "cxx17_ratchet", "cxx17_ratchet.py"),
                                      "--report", "--worktree"]).stdout)
        sha = _git_util.commit_paths(ROOT, kept, commit_message(number, summary, totals, args.rewrite,
                                                                 "compile" if compile_proof else "objhash", args.corpus), TRAILER)
        runner([sys.executable, os.path.join(ROOT, "tools", "cxx17_ratchet", "cxx17_ratchet.py"), "--check", "--tighten"])
        if "tools/cxx17_ratchet/baseline.json" in _git_util.dirty_paths(ROOT, tracked_only=True):
            sha += " + " + _git_util.commit_paths(ROOT, ["tools/cxx17_ratchet/baseline.json"],
                                                  f"cxx17 ratchet baseline tightened after tidy batch {number}", TRAILER)
        if args.log_identity:
            err = ensure_deployed(runner) or capture_logs(logs_after, runner)
            if err:
                return "failed", f"committed {sha}, but the after-logs could not be captured: " + err
            only_before, only_after, tags_equal = log_identity.compare(logs_before, logs_after)
            if only_before or only_after or not tags_equal:
                return "failed", (f"committed {sha}, but the game logs differ: {len(only_before)} line shape(s) only before, "
                                  f"{len(only_after)} only after, tag set {'equal' if tags_equal else 'DIFFERENT'}; first: "
                                  + (only_before + only_after)[0][:200])
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
    ap.add_argument("--proof", choices=("auto", "objhash", "compile"), default="auto",
                    help="objhash: every rebuilt object must be byte-identical; compile: the files build (printf conversions change the code by "
                         "design; their proof is the corpus, the census and the log comparison). auto = compile for the COMPILE_ONLY rewriters")
    ap.add_argument("--corpus", action="store_true", help="record the converted format pairs in tests/unit/printf_corpus.jsonl and regenerate the doctest")
    ap.add_argument("--log-identity", action="store_true",
                    help="build+deploy+smoke (mc2_03, mc2_24) before the batch and again after its commit; fail when the game logs differ by line shape")
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
