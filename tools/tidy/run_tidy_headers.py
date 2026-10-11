#!/usr/bin/env python3
"""run_tidy_headers - phase 2 of the clang-tidy safe set: rewrite HEADERS and prove every translation unit that includes them.

WHAT: for a batch of headers, (1) find every object that includes each header (ninja's dependency log), (2) hash those objects, (3) run the
safe checks on one including TU with diagnostics restricted to the batch's headers (clang-tidy -line-filter), (4) rebuild every affected
object, (5) compare each with tools/objhash.py. A header whose includers are all identical is kept; any header whose includers differ, or fail
to compile is bisected out: the headers whose including objects are all identical are kept, the rest are reverted. A header that
defines an aggregate by typedef is not offered modernize-use-using (MSVC rejects the rewrite). Files the lane-2 branch changed are excluded
(--exclude-lane).
WHY: a header edit reaches every TU that includes it, so the proof set is the union of those TUs, not one object. The batch is sized by that
union (--max-objs) so a rewrite of a widely included header does not turn into a full rebuild.

  py -3 tools/tidy/run_tidy_headers.py --tree build-alltargets --dir mclib --max-objs 300
  py -3 tools/tidy/run_tidy_headers.py --tree build-alltargets mclib/sortedsocket.hpp mclib/stuff/link.hpp
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_safe as rts  # noqa: E402
import pick_batch  # noqa: E402
import tidy_lock  # noqa: E402

objhash = rts.objhash
console_child = rts.console_child
ROOT = rts.ROOT


def includer_map(tree_abs):
    """-> {repo-relative header: set(object targets relative to the tree)} from `ninja -t deps`."""
    import configure_ninja as cn
    out = subprocess.run([cn.NINJA, "-C", tree_abs, "-t", "deps"], capture_output=True, text=True, errors="replace",
                         **console_child.child_kwargs(redirected=True)).stdout
    by_header, cur = {}, None
    for line in out.splitlines():
        if not line.strip():
            cur = None
        elif not line.startswith(" "):
            cur = line.split(":", 1)[0].replace("\\", "/")
        elif cur is not None:
            hp = os.path.normpath(os.path.join(tree_abs, line.strip())).replace("\\", "/")
            try:
                rel = os.path.relpath(hp, ROOT).replace("\\", "/")
            except ValueError:      # a system header on another drive
                continue
            if not rel.startswith(".."):
                by_header.setdefault(rel, set()).add(cur)
    return by_header


def target_to_source(tree_abs):
    """-> {object target (relative to the tree): repo-relative source}."""
    with open(os.path.join(tree_abs, "compile_commands.json"), encoding="utf-8") as fh:
        db = json.load(fh)
    out = {}
    for e in db:
        obj = os.path.normpath(os.path.join(e["directory"], e["output"]))
        src = e["file"] if os.path.isabs(e["file"]) else os.path.join(e["directory"], e["file"])
        out[os.path.relpath(obj, tree_abs).replace("\\", "/")] = os.path.relpath(os.path.normpath(src), ROOT).replace("\\", "/")
    return out


HEADER_SITE = pick_batch.SITE


def candidate_headers(dirs, includers, dirty, rewrite=None):
    """Tracked headers under `dirs` with a rewritable site, not excluded or dirty, that some object includes."""
    out = subprocess.run(["git", "-C", ROOT, "ls-files"] + [d.rstrip("/") + "/" for d in dirs], capture_output=True, text=True,
                         **console_child.child_kwargs(redirected=True)).stdout.split()
    cands = []
    for rel in out:
        if not rel.lower().endswith((".h", ".hpp", ".inl")) or rts.is_excluded(rel) or rel in dirty or rel not in includers:
            continue
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as fh:
            text = fh.read()
            if pick_batch._rewrite_site(text, rewrite) if rewrite else pick_batch.has_site(text):
                cands.append((len(includers[rel]), os.path.getsize(os.path.join(ROOT, rel)), rel))
    cands.sort()
    return cands


def line_filter(headers):
    return json.dumps([{"name": os.path.join(ROOT, h).replace("/", "\\")} for h in headers])


TYPEDEF_AGGREGATE = re.compile(r"\btypedef\s+(?:struct|union|enum|class)\b")


def checks_for(text: str) -> tuple:
    """The safe checks to run on a header with this text.

    WHY: modernize-use-using turns `typedef struct {...} T;` into `using T = struct {...};`, which MSVC rejects in every including TU.
    A header that defines an aggregate through a typedef is therefore not offered that check (its other rewrites still apply)."""
    if TYPEDEF_AGGREGATE.search(text):
        return tuple(c for c in rts.SAFE_CHECKS if c != "modernize-use-using")
    return rts.SAFE_CHECKS


def read_bytes(rel: str) -> bytes:
    with open(os.path.join(ROOT, rel), "rb") as fh:
        return fh.read()


class Proof:
    """Applies a subset of the proposed header edits to the tree and compares every affected object with its pre-edit digest."""

    def __init__(self, tree_abs, includers, orig, edited, jobs, sources=None):
        self.tree_abs, self.includers, self.orig, self.edited, self.jobs = tree_abs, includers, orig, edited, jobs
        self.sources = sources or {}          # object target -> repo-relative source, for the nondeterminism check
        self.headers = sorted(edited)
        self.objs = sorted({o for h in self.headers for o in includers[h]})
        self.on_disk = dict(orig)
        self.before = {}
        self.noisy = set()                    # objects that differ between two compiles of identical input
        self.deterministic = set()            # objects already checked and found stable
        self.verified = None                  # (frozenset of applied headers) of the last probe that built clean

    def baseline(self) -> int:
        rc, _ = rts.ninja_objects_targets(self.tree_abs, self.objs, self.jobs)
        if rc:
            return rc
        self.before = {o: objhash.obj_digest(os.path.join(self.tree_abs, o)) for o in self.objs}
        return 0

    def apply(self, subset) -> None:
        """Make the tree hold the edited text for `subset` and the original text for every other proposed header."""
        for h in self.headers:
            want = self.edited[h] if h in subset else self.orig[h]
            if self.on_disk[h] != want:
                with open(os.path.join(ROOT, h), "wb") as fh:
                    fh.write(want)
                self.on_disk[h] = want

    def bad_objects(self) -> set:
        """Build every object that includes a proposed header; -> those that fail to compile or differ from the baseline."""
        rc, failed = rts.ninja_objects_targets(self.tree_abs, self.objs, self.jobs)
        bad = {f.replace("\\", "/") for f in failed}
        for o in self.objs:
            if o in bad or o in self.noisy:
                continue
            now = objhash.obj_digest(os.path.join(self.tree_abs, o))
            if now is None or now["code_sha256"] != self.before[o]["code_sha256"]:
                bad.add(o)
        return bad

    def find_noisy(self, bad) -> set:
        """-> the objects in `bad` that compile to different bytes from the same input (an embedded timestamp, an address, ...).

        Each is recompiled twice with its source touched and nothing else changed. A compile failure is not noise. Such an object proves
        nothing about any header, so it is left out of the comparison instead of reverting every header it includes."""
        noisy = set()
        for o in sorted(bad):
            src = self.sources.get(o)
            if not src or o in self.deterministic:
                continue
            digests = []
            for _ in range(2):
                os.utime(os.path.join(ROOT, src), None)
                rc, failed = rts.ninja_objects_targets(self.tree_abs, [o], self.jobs)
                if failed:
                    break
                d = objhash.obj_digest(os.path.join(self.tree_abs, o))
                digests.append(d and d["code_sha256"])
            if len(digests) == 2 and digests[0] != digests[1]:
                noisy.add(o)
            elif len(digests) == 2:
                self.deterministic.add(o)
        return noisy

    def solve(self):
        """Keep every header whose including objects are all identical; bisect the suspects of a failure. -> (kept, [(header, why)])."""
        keep, rejected = [], []

        def probe(trial):
            t0 = time.monotonic()
            self.apply(set(keep) | set(trial))
            bad = self.bad_objects()
            if bad:
                found = self.find_noisy(bad)
                if found:
                    self.noisy |= found
                    for o in sorted(found):
                        print(f"  NOISY {o}: compiles to different bytes from identical input; ignored", flush=True)
                    bad = self.bad_objects()
            print(f"  probe {len(trial)} header(s): {len(bad)} differing/failing object(s), {time.monotonic() - t0:.0f} s", flush=True)
            if not bad:
                self.verified = frozenset(set(keep) | set(trial))
            return bad

        def go(trial):
            if not trial:
                return
            bad = probe(trial)
            if not bad:
                keep.extend(trial)
                return
            suspects = [h for h in trial if self.includers[h] & bad] or list(trial)
            keep.extend(h for h in trial if h not in suspects)   # every object including them was identical
            if len(suspects) == 1:
                h = suspects[0]
                names = ", ".join(sorted(self.includers[h] & bad)[:3])
                rejected.append((h, f"{len(self.includers[h] & bad)} of {len(self.includers[h])} includers differ or fail to compile ({names})"))
                return
            mid = len(suspects) // 2
            go(suspects[:mid])
            go(suspects[mid:])

        go(list(self.headers))
        self.apply(set(keep))
        # The last clean probe already built exactly this state, so a full set that passed at once needs no second build.
        final_bad = set() if self.verified == frozenset(keep) else self.bad_objects()
        if final_bad:      # the accepted headers interact; nothing is kept
            rejected += [(h, "kept set failed its final build") for h in keep]
            keep = []
            self.apply(set())
            self.bad_objects()
        return keep, rejected


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("headers", nargs="*")
    ap.add_argument("--dir", action="append", default=[])
    ap.add_argument("--tree", default="build-alltargets")
    ap.add_argument("--max-objs", type=int, default=300, help="stop adding headers once the union of includers exceeds this")
    ap.add_argument("--max-headers", type=int, default=50)
    ap.add_argument("--max-header-includers", type=int, default=200,
                    help="skip a header included by more objects than this (its proof is a near-full rebuild for a handful of sites)")
    ap.add_argument("--done", help="ledger of headers already tried (skipped; picked ones are appended)")
    ap.add_argument("--build-jobs", type=int, default=6)
    ap.add_argument("--dry-run", action="store_true", help="list the batch and its proof set, change nothing")
    ap.add_argument("--loop", action="store_true",
                    help="keep running batches until no candidate header is left, a batch cannot be built, or a stop is requested")
    ap.add_argument("--max-batches", type=int, default=0, help="with --loop: stop after this many batches (0 = no limit)")
    ap.add_argument("--stop-after-batch", action="store_true",
                    help="do not run a batch: ask the tool that holds this tree's lock to exit after its current batch (the stop "
                         "request is the file <tree>/.tidy.stop)")
    ap.add_argument("--rewrite", choices=rts.REWRITERS, help="apply this text transform to each header instead of running clang-tidy")
    rts.add_lane_args(ap)
    return ap


def run_batch(args):
    """One batch. -> (exit code, True when no candidate header is left)."""
    tree_abs = os.path.join(ROOT, args.tree)
    includers = includer_map(tree_abs)
    t2s = target_to_source(tree_abs)
    dirty = rts.dirty_files()
    done = set()
    if args.done and os.path.exists(args.done):
        with open(args.done, encoding="utf-8") as fh:
            done = {ln.strip() for ln in fh if ln.strip()}

    if args.headers:
        headers = [h.replace("\\", "/") for h in args.headers]
    else:
        headers, union = [], set()
        for n, _sz, rel in candidate_headers(args.dir, includers, dirty, args.rewrite):
            if rel in done or len(includers[rel]) > args.max_header_includers:
                continue
            new_union = union | includers[rel]
            if headers and len(new_union) > args.max_objs:
                continue
            headers.append(rel)
            union = new_union
            if len(headers) >= args.max_headers:
                break
    if not headers:
        print("run_tidy_headers: no candidate headers")
        return 0, True
    if args.lane_excluded_note:
        print(args.lane_excluded_note)
    if args.done and not args.dry_run:
        with open(args.done, "a", encoding="utf-8", newline="\n") as fh:
            fh.writelines(h + "\n" for h in headers)

    print(f"run_tidy_headers: {len(headers)} header(s) selected, {len({o for h in headers for o in includers.get(h, ())})} "
          f"object(s) in their include closure", flush=True)
    if args.dry_run:
        for h in headers:
            print(f"  {h}  ({len(includers.get(h, ()))} objects)")
        return 0, True

    # Propose the edits first (clang-tidy only needs the sources), keep the original and edited text, and put the originals back:
    # the baseline build and the per-header proof then run over the headers that actually changed.
    exe = rts.clang_tidy_exe()
    orig = {h: read_bytes(h) for h in headers}
    mtimes = {h: os.stat(os.path.join(ROOT, h)).st_mtime_ns for h in headers}
    edited = {}
    for h in headers:
        # clang-tidy applies no fix-its from a TU it cannot parse (MSVC-only constructs), so try the including TUs smallest first until
        # one rewrites the header (or none does).
        if args.rewrite:
            rts.rewrite_hook(args.rewrite)(["rewrite", h])
            if read_bytes(h) != orig[h]:
                edited[h] = read_bytes(h)
                with open(os.path.join(ROOT, h), "wb") as fh:
                    fh.write(orig[h])
                os.utime(os.path.join(ROOT, h), ns=(mtimes[h], mtimes[h]))
            continue
        checks = checks_for(orig[h].decode("utf-8", errors="replace"))
        tus = sorted((os.path.getsize(os.path.join(ROOT, t2s[o])), t2s[o]) for o in includers.get(h, ()) if o in t2s
                     and not rts.is_excluded(t2s[o]) and t2s[o].lower().endswith((".cpp", ".cc", ".cxx")))
        seen = set()
        for _sz, tu in tus:
            if tu in seen:
                continue
            seen.add(tu)
            if len(seen) > 6:
                break
            cmd = rts.tidy_command(exe, args.tree, tu, checks=checks, fix=True)
            cmd.insert(-1, "--line-filter=" + line_filter([h]))
            cmd = [c for c in cmd if not c.startswith("--header-filter")] + ["--header-filter=.*"]
            subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
            if rts.git("diff", "--numstat", "--", tu).strip():        # the line filter should prevent this; never keep a TU edit
                rts.restore_file(tu, os.stat(os.path.join(ROOT, tu)).st_mtime_ns)
            if read_bytes(h) != orig[h]:
                edited[h] = read_bytes(h)
                break
        if h in edited:
            with open(os.path.join(ROOT, h), "wb") as fh:
                fh.write(orig[h])
            os.utime(os.path.join(ROOT, h), ns=(mtimes[h], mtimes[h]))
    for h in sorted(edited):
        print(f"EDIT  {h}  ({len(includers[h])} objects)")
    if not edited:
        print("run_tidy_headers: no rewrites needed")
        return 0, False

    proof = Proof(tree_abs, includers, orig, edited, args.build_jobs, sources=t2s)
    if proof.baseline():
        print("run_tidy_headers: the base build of the proof set failed; refusing to edit", file=sys.stderr)
        return 2, False
    kept, rejected = proof.solve()
    for h, why in rejected:
        rts.restore_file(h, mtimes[h])
        print(f"REVERT {h}  {why}")
    print(f"run_tidy_headers: {len(kept)} header(s) proven identical over {len({o for h in kept for o in includers[h]})} "
          f"object(s), {len(rejected)} reverted")
    if kept:
        print("kept (commit by pathspec): " + " ".join(kept))
    return (1 if rejected else 0), False


def main(argv=None) -> int:
    sys.stdout.reconfigure(line_buffering=True)
    args = build_parser().parse_args(argv)
    rts.apply_lane_args(args)
    tree_abs = os.path.join(ROOT, args.tree)
    if args.stop_after_batch:
        print("run_tidy_headers: " + tidy_lock.request_stop(tree_abs))
        return 0
    if args.dry_run:
        return run_batch(args)[0]
    try:
        with tidy_lock.TidyLock(tree_abs):
            tidy_lock.clear_stop(tree_abs)
            batches = 0
            while True:
                rc, exhausted = run_batch(args)
                batches += 1
                if (not args.loop or exhausted or rc == 2 or tidy_lock.stop_requested(tree_abs)
                        or (args.max_batches and batches >= args.max_batches)):
                    break
            tidy_lock.clear_stop(tree_abs)
    except tidy_lock.TidyBusy as e:
        print(f"run_tidy_headers: {e}", file=sys.stderr)
        return 3
    return rc


if __name__ == "__main__":
    sys.exit(main())
