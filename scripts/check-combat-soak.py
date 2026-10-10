#!/usr/bin/env python3
"""check-combat-soak.py — COMBAT-SOAK-GATE-1

Headless combat regression gate. Runs one or more "oracle" missions with the COMBAT
diagnostic tag (COMBAT-TRACE-1) and asserts that AI combat actually occurred:
weapon HITS and KILLS (disabled mechs) above conservative thresholds.

This protects the brain/ABL-dispatch arc: a future change that silently breaks AI
engagement (mechs stop firing / dying) fails this gate, instead of passing a smoke
that only checks "the mission loaded without crashing". (The smoke's own `Δ destroys`
counter does NOT count disables — a disabled mech IS the kill — so it cannot be used
for this; the COMBAT trace is the authoritative signal.)

Oracles (both verified to fight headlessly, no player input):
  - mc2_24  (stock multi-mech; no mods)             ~14 hit / 4 disabled per 90s
  - torrin  (DarkRain, deps mc2x-compat,cveg)        ~51 hit / 6 disabled per 90s

Combat is stochastic; thresholds are set well below observed (hits>=5, kills>=1).

Convention: scripts/check-*.py — exit 0 = all oracles fought; 1 = a gate failed;
2 = harness error (run_smoke / exe / trace-file problem).

Usage:
  py -3 scripts/check-combat-soak.py --exe <deployed mc2.exe> [--duration 90]
        [--mission torrin] [--only mc2_24] [--quiet] [--json out.json]
"""

import argparse
import json
import os
import subprocess
import sys
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

# Oracle missions known to produce autonomous headless combat.
ORACLES = [
    {"mission": "mc2_24", "mod": None,       "deps": None,                "min_hits": 5, "min_kills": 1},
    {"mission": "torrin", "mod": "DarkRain", "deps": "mc2x-compat,cveg",  "min_hits": 5, "min_kills": 1},
]


def count_combat_events(jsonl_path):
    """Return (hits, disabled, destroyed) from a COMBAT-tagged diagnostic JSONL."""
    hits = disabled = destroyed = 0
    try:
        with open(jsonl_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if '"tag":"COMBAT"' not in line:
                    continue
                if '"ev":"hit"' in line:
                    hits += 1
                elif '"ev":"disabled"' in line:
                    disabled += 1
                elif '"ev":"destroyed"' in line:
                    destroyed += 1
    except OSError:
        return None
    return (hits, disabled, destroyed)


def run_oracle(oracle, exe, root, duration, quiet):
    """Run one oracle mission with the COMBAT trace; return a result dict."""
    mission = oracle["mission"]
    trace_rel = os.path.join("debug_state", "combat_soak_%s.jsonl" % mission)
    exe_dir = os.path.dirname(os.path.abspath(exe))
    jsonl_path = os.path.join(exe_dir, trace_rel)
    # Stale-proof: remove any prior trace so a failed launch can't read an old pass.
    try:
        os.remove(jsonl_path)
    except OSError:
        pass

    env = dict(os.environ)
    env["MC2_DEBUG_STATE_DUMP"] = "1"
    env["MC2_DIAGNOSTIC_TRACE_FILE"] = trace_rel
    env["MC2_DIAG_TAGS"] = "CONFIG,BUILD,DEVICE,COMBAT"
    if oracle["mod"]:
        env["MC2_ACTIVE_MOD"] = oracle["mod"]
        env["MC2_MOD_DEPS"] = oracle["deps"]

    cmd = [sys.executable, os.path.join(root, "scripts", "run_smoke.py"),
           "--exe", exe, "--mission", mission,
           "--duration", str(duration), "--keep-logs"]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    smoke_ok = (proc.returncode == 0)

    counts = count_combat_events(jsonl_path)
    if counts is None:
        return {"mission": mission, "error": "trace file not found: %s" % jsonl_path,
                "smoke_ok": smoke_ok, "passed": False}
    hits, dis, des = counts
    passed = (hits >= oracle["min_hits"]) and (dis >= oracle["min_kills"])
    return {"mission": mission, "hits": hits, "disabled": dis, "destroyed": des,
            "min_hits": oracle["min_hits"], "min_kills": oracle["min_kills"],
            "smoke_ok": smoke_ok, "passed": passed}


def main():
    ap = argparse.ArgumentParser(description="Headless combat regression gate (COMBAT-SOAK-GATE-1)")
    ap.add_argument("--exe", required=True, help="Deployed mc2.exe to soak")
    ap.add_argument("--root", default=None, help="Worktree root (default: two levels up from this script)")
    ap.add_argument("--duration", type=int, default=90, help="Seconds per oracle (default 90)")
    ap.add_argument("--only", action="append", default=None,
                    help="Restrict to these oracle mission(s); repeatable")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--json", default=None, help="Write structured results to this path")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isfile(args.exe):
        print("FAIL  combat_soak  exe not found: %s" % args.exe)
        return 2

    oracles = ORACLES
    if args.only:
        oracles = [o for o in ORACLES if o["mission"] in args.only]
        if not oracles:
            print("FAIL  combat_soak  no oracle matched --only %s" % args.only)
            return 2

    results = []
    for o in oracles:
        r = run_oracle(o, args.exe, root, args.duration, args.quiet)
        results.append(r)
        if not args.quiet:
            if "error" in r:
                print("FAIL  combat_soak  %-8s  %s" % (r["mission"], r["error"]))
            else:
                verdict = "PASS" if r["passed"] else "FAIL"
                print("%s  combat_soak  %-8s  hits=%d (>=%d)  kills=%d (>=%d)  destroyed=%d  smoke_ok=%s"
                      % (verdict, r["mission"], r["hits"], r["min_hits"],
                         r["disabled"], r["min_kills"], r["destroyed"], r["smoke_ok"]))

    n_fail = sum(1 for r in results if not r.get("passed"))
    if not args.quiet:
        print("\ncombat_soak: %d oracle(s), %d passed, %d failed"
              % (len(results), len(results) - n_fail, n_fail))

    if args.json:
        with open(args.json, "w") as jf:
            json.dump({"result": "FAIL" if n_fail else "PASS", "oracles": results}, jf, indent=2)

    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
