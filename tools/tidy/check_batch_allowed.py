#!/usr/bin/env python3
"""check_batch_allowed - before a rewrite_defines batch: which candidate headers would the batch SKIP, and why.

WHAT: runs the planner of rewrite_defines.py (the same code, so the answer cannot drift) with the same ledgers and prints the files that would run, every file with
eligible sites that would not (tainted by an object the taint ledger lists, already in the done ledger, dirty, never-touch, no including object, too many includers, wrong scope),
and the tainted objects ranked by how many headers each one hides.
WHY: ten headers were hidden behind ONE flaky object (a differing object taints every header it includes) for most of a session without anyone noticing; the ledger is
silent by design, so the way to see its cost is to ask before the batch.

  py -3 tools/tidy/check_batch_allowed.py --tree build-alltargets --scope all --taint-file T --done D [--max-header-includers N] [--strict]
Exit 0; with --strict exit 1 when any header is hidden by the taint ledger.
"""
from __future__ import annotations

import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rewrite_defines as rd  # noqa: E402
import run_tidy_headers as rth  # noqa: E402
import run_tidy_safe as rts  # noqa: E402


def hidden_by_object(skips):
    """-> [(object, [headers])] for the taint-ledger skips, the object hiding the most headers first."""
    by_obj = collections.defaultdict(list)
    for rel, kind, detail, _n in skips:
        if kind == "tainted":
            for o in detail.split(","):
                by_obj[o].append(rel)
    return sorted(((o, sorted(hs)) for o, hs in by_obj.items()), key=lambda x: (-len(x[1]), x[0]))


def report(plan, skips, done=(), taint=()):
    """-> the report lines for a plan [(nobjs, rel, sites, objs)] and a skip list [(rel, kind, detail, nsites)]; files in `done` count as skipped."""
    done = set(done)
    runs = [p for p in plan if p[1] not in done]
    skipped = [(p[1], "done", "in the done ledger", len(p[2])) for p in plan if p[1] in done] + list(skips)
    lines = ["would run: %d file(s), %d site(s)" % (len(runs), sum(len(p[2]) for p in runs))]
    lines += ["  run   %s  %d site(s), %d object(s)" % (p[1], len(p[2]), p[0]) for p in runs]
    lines.append("would skip: %d file(s), %d site(s)" % (len(skipped), sum(s[3] for s in skipped)))
    order = {"tainted": 0, "done": 2}
    for rel, kind, detail, n in sorted(skipped, key=lambda s: (order.get(s[1], 1), s[1], s[0])):
        lines.append("  skip  %-18s %s  %d site(s)%s" % (kind, rel, n, ("  <- " + detail) if detail and kind == "tainted" else ("  (" + detail + ")" if detail else "")))
    hidden = hidden_by_object(skips)
    if hidden:
        lines.append("taint ledger (%d object(s) listed) hides %d header(s); the objects hiding the most:" % (len(set(taint)), sum(1 for s in skips if s[1] == "tainted")))
        for o, hs in hidden[:10]:
            lines.append("  %s hides %d header(s): %s" % (o, len(hs), ", ".join(hs[:12]) + (" ..." if len(hs) > 12 else "")))
    return lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scope", choices=("cpp", "header", "all"), default="all")
    ap.add_argument("--tree", default="build64-ninja")
    ap.add_argument("--max-header-includers", type=int, default=200)
    ap.add_argument("--done")
    ap.add_argument("--taint-file")
    ap.add_argument("--skip-dirty-in", default="A:/Games/mc2-main")
    ap.add_argument("--strict", action="store_true")
    rts.add_lane_args(ap)
    a = ap.parse_args(argv)
    rts.apply_lane_args(a)
    tree_abs = os.path.join(rts.ROOT, a.tree)
    includers = rth.includer_map(tree_abs)
    a.s2t = {v: k for k, v in rth.target_to_source(tree_abs).items()}
    _paths, files, sites, uses = rd.load_world()
    a.taint = rd.load_taint(a.taint_file)
    done = {l.strip() for l in open(a.done, encoding="utf-8") if l.strip()} if a.done and os.path.exists(a.done) else set()
    dirty = rts.dirty_files() | rd.other_worktree_dirty(a.skip_dirty_in)
    skips = []
    plan = rd.plan_batches(files, sites, uses, includers, a, dirty, skips)
    print("check_batch_allowed: tree %s, scope %s, taint ledger %d object(s), done ledger %d file(s)" % (a.tree, a.scope, len(a.taint), len(done)))
    print("\n".join(report(plan, skips, done, a.taint)))
    return 1 if a.strict and any(s[1] == "tainted" for s in skips) else 0


if __name__ == "__main__":
    sys.exit(main())
