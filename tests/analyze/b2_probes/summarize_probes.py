#!/usr/bin/env python3
"""summarize_probes - read the [B2P1] / [B2P2] lines of probe-exe mission logs and print the tables that go into cxx23-step4-b2-notes.md section 8b.

WHAT: for each log (one per mission run) it prints, per SortList site, the call count, the largest filled prefix, the calls with ties, the calls whose CRT
qsort order differs from "score, then fill slot", NaN / -inf / sentinel-leak calls and the FIRST sim frame at which a tie / a difference was seen; and per A*
solver the solve count, the largest open list, find()==0 events, the largest index passed to change(), changes beyond the live heap or the buffer, the
bug hits of GlobalMap::propogateCost with the first hit frame, and the minimum key. A probe line belongs to the sim frame of the NEXT [SIMID v1] line.
WHY: a site with zero calls is UNEXERCISED and is printed as such, never as "no divergence"; every claim is bounded by the frames the run reached.

  py -3 tests/analyze/b2_probes/summarize_probes.py --window 1000 mc2_03=run_a/mc2_03.log mc2_10=...
"""
from __future__ import annotations

import argparse
import re
import sys

SIMID = re.compile(r"\[SIMID v1\] simframe=(\d+)")
P1 = re.compile(r"\[B2P1\] simframe=(\d+) site=(\S+) calls=(\d+) maxn=(\d+) ties=(\d+) diff=(\d+) nan=(\d+) inf=(\d+) leak=(\d+) cap=(\d+)")
P2 = re.compile(r"\[B2P2\] solver=(\S+) inserts=(\d+) removes=(\d+) changes=(\d+) finds=(\d+) find0=(\d+) maxopen=(\d+) chgmax=(\d+) chg_gt_n=(\d+) chg_ge_cap=(\d+) minkey=(-?\d+) cap=(\d+) ophash=([0-9a-f]+)")
HIT = re.compile(r"\[B2P2 hit\] solver=(\S+) door=(-?\d+) openIndex=(-?\d+) numItems=(\d+) stale=(\d) past5002=(\d) total=(\d+)")
SITES = ("getContacts", "getMoversWithinRadius", "calcOptimalRange", "sortWeapons_attack", "sortWeapons_value")
SOLVERS = ("globalmap", "movemap", "movemap_jump", "movemap_escape", "goalmap")


def summarize(name: str, path: str, window: int) -> None:
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    # Assign every probe line to the frame of the next SIMID line.
    frames = [int(m[1]) for m in (SIMID.search(ln) for ln in lines) if m]
    last_frame = max(frames) if frames else 0
    pending: list[tuple[str, str]] = []
    p1_last: dict[str, tuple] = {}
    p1_first_tie: dict[str, int] = {}
    p1_first_diff: dict[str, int] = {}
    p1_in_window: dict[str, tuple] = {}
    p2 = {s: dict(solves=0, maxopen=0, find0=0, chgmax=0, gt_n=0, ge_cap=0, minkey=None, over5001=0, ophashes=0) for s in SOLVERS}
    hits = []
    for ln in lines:
        m = SIMID.search(ln)
        if m:
            frame = int(m[1])
            for kind, text in pending:
                if kind == "hit":
                    h = HIT.search(text)
                    hits.append((frame, h[1], int(h[2]), int(h[3]), int(h[4]), h[5] == "1", h[6] == "1"))
            pending.clear()
            continue
        if "[B2P1]" in ln:
            m1 = P1.search(ln)
            if m1:
                frame, site = int(m1[1]), m1[2]
                row = tuple(int(x) for x in m1.groups()[2:])  # calls maxn ties diff nan inf leak cap
                p1_last[site] = row
                if row[2] > 0 and site not in p1_first_tie:
                    p1_first_tie[site] = frame
                if row[3] > 0 and site not in p1_first_diff:
                    p1_first_diff[site] = frame
                if frame <= window:
                    p1_in_window[site] = row
        elif "[B2P2 hit]" in ln:
            pending.append(("hit", ln))
        elif "[B2P2]" in ln:
            m2 = P2.search(ln)
            if m2 and m2[1] in p2:
                s = p2[m2[1]]
                s["solves"] += 1
                s["maxopen"] = max(s["maxopen"], int(m2[7]))
                s["find0"] += int(m2[6])
                s["chgmax"] = max(s["chgmax"], int(m2[8]))
                s["gt_n"] += int(m2[9])
                s["ge_cap"] += int(m2[10])
                key = int(m2[11])
                s["minkey"] = key if s["minkey"] is None else min(s["minkey"], key)
                s["over5001"] += int(m2[7]) > 5001
    print(f"### {name}: {len(frames)} sim frames, last {last_frame}; baseline window {window}")
    print("P1 (SortList):")
    print("| site | calls | max n | ties | qsort != rule | NaN | inf | leak | first tie frame | first diff frame | diff within window |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for site in SITES:
        if site not in p1_last:
            print(f"| {site} | 0 (UNEXERCISED) | | | | | | | | | |")
            continue
        calls, maxn, ties, diff, nan, inf, leak, _cap = p1_last[site]
        win = p1_in_window.get(site, (0,) * 8)
        print(f"| {site} | {calls} | {maxn} | {ties} | {diff} | {nan} | {inf} | {leak} | {p1_first_tie.get(site, '-')} | {p1_first_diff.get(site, '-')} | {win[3]} of {win[0]} |")
    print("P2 (A* open list):")
    print("| solver | solves | max open | find()==0 | largest index to change | change > live n | change >= buffer | min key | solves > 5001 |")
    print("|---|---|---|---|---|---|---|---|---|")
    for solver in SOLVERS:
        s = p2[solver]
        if not s["solves"]:
            print(f"| {solver} | 0 (UNEXERCISED) | | | | | | | |")
        else:
            print(f"| {solver} | {s['solves']} | {s['maxopen']} | {s['find0']} | {s['chgmax']} | {s['gt_n']} | {s['ge_cap']} | {s['minkey']} | {s['over5001']} |")
    in_window = [h for h in hits if h[0] <= window]
    print(f"propogateCost hits (door != openIndex): {len(hits)} total, {len(in_window)} within frame {window}; "
          f"stale {sum(1 for h in hits if h[5])}, door >= 5002 {sum(1 for h in hits if h[6])}"
          + (f"; first at frame {hits[0][0]} (door {hits[0][2]}, openIndex {hits[0][3]}, numItems {hits[0][4]})" if hits else ""))
    print()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", metavar="NAME=LOG")
    ap.add_argument("--window", type=int, default=1000, help="the first sim frames a determinism baseline covers (default 1000)")
    args = ap.parse_args()
    for item in args.logs:
        name, _, path = item.partition("=")
        summarize(name, path or name, args.window)
    return 0


if __name__ == "__main__":
    sys.exit(main())
