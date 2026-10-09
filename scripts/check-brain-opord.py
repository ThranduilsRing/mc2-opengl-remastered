#!/usr/bin/env python3
"""check-brain-opord.py — BRAIN-OPORD-GATE-1

Headless regression gate for the declarative TechBrain OPORD pipeline (parse -> consumer ->
runtime -> drive/engage). Runs the converted carver_v_enhanced mc2_24 with the full brain bundle
and asserts the key runtime signals fire:

  - the mission.fit OPORD consumer activated a meaningful number of Patrol + Guard units,
  - those patrols actually drive (BRAIN_PATROL_KICK),
  - units engage detected threats (BRAIN_ENGAGE),
  - real combat occurs (COMBAT-trace weapon hits).

This protects the whole declarative path that carver exercises directly (Patrol/Guard/engage).
The deeper per-type behaviors (MoveTo/Withdraw/Ambush/Attack/Scout) were verified once by rewriting
the mission's PrimaryOPORD types and asserting each unique trace (KICK+DONE+PROGRESS for finite
moves, SENTRY_WAKE for Ambush, high engage for Attack); this gate keeps the common path green.

PREREQUISITE (like the combat-soak oracles): the carver_v_enhanced mission must be staged as a mod
named by --mod (default carver_probe) at <exe_dir>/mods/<mod>/data/missions/<mission>.fit. If it is
not present the gate is SKIPPED (exit 2) rather than failed — the environment is not set up.

Convention: scripts/check-*.py — exit 0 = pass, 1 = a gate failed, 2 = harness/env error or skip.

Usage:
  py -3 scripts/check-brain-opord.py --exe <deployed mc2.exe> [--duration 240]
"""

import argparse
import os
import re
import subprocess
import sys

BUNDLE = {
    "MC2_BRAIN_MISSIONFIT_OPORD": "1",
    "MC2_BRAIN_INLINE_EMPTY_SKIP": "1",
    "MC2_BRAIN_TASKQ": "1",
    "MC2_BRAIN_RUNTIME": "1",
    "MC2_BRAIN_RUNTIME_APPLY": "1",
    "MC2_BRAIN_PATROL": "1",
    "MC2_BRAIN_INTENT_QUEUE": "1",
    "MC2_BRAIN_ENGAGE": "1",
}

# Conservative thresholds (carver mc2_24 observed ~44 patrol / 20 guard, kicks ~38, hundreds of
# engage emits + hits over 240s). Set well below observed so stochastic combat does not flake.
MIN_PATROL_APPLIED = 25
MIN_GUARD_APPLIED  = 10
MIN_PATROL_KICKS   = 20
MIN_ENGAGE_EMITS   = 10
MIN_HITS           = 15


def latest_artifact_log(root, mission):
    base = os.path.join(root, "tests", "smoke", "artifacts")
    if not os.path.isdir(base):
        return None
    dirs = [os.path.join(base, d) for d in os.listdir(base)]
    dirs = [d for d in dirs if os.path.isdir(d)]
    if not dirs:
        return None
    newest = max(dirs, key=os.path.getmtime)
    log = os.path.join(newest, "%s.log" % mission)
    return log if os.path.isfile(log) else None


def main():
    ap = argparse.ArgumentParser(description="Declarative-brain OPORD regression gate")
    ap.add_argument("--exe", required=True, help="Deployed mc2.exe")
    ap.add_argument("--root", default=None, help="Worktree root (default: parent of scripts/)")
    ap.add_argument("--mod", default="carver_probe", help="Staged carver mod name")
    ap.add_argument("--mission", default="mc2_24")
    ap.add_argument("--duration", type=int, default=240,
                    help="Seconds (>=180; patrols must close + the scripted engagement must fire)")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isfile(args.exe):
        print("SKIP  brain_opord  exe not found: %s" % args.exe)
        return 2

    exe_dir = os.path.dirname(os.path.abspath(args.exe))
    mfit = os.path.join(exe_dir, "mods", args.mod, "data", "missions", "%s.fit" % args.mission)
    if not os.path.isfile(mfit):
        print("SKIP  brain_opord  carver mod not staged: %s "
              "(stage the carver_v_enhanced mission as mod '%s')" % (mfit, args.mod))
        return 2

    trace_rel = os.path.join("debug_state", "brain_opord_%s.jsonl" % args.mission)
    jsonl = os.path.join(exe_dir, trace_rel)
    try:
        os.remove(jsonl)
    except OSError:
        pass

    env = dict(os.environ)
    env.update(BUNDLE)
    env["MC2_ACTIVE_MOD"] = args.mod
    env["MC2_DEBUG_STATE_DUMP"] = "1"
    env["MC2_DIAGNOSTIC_TRACE_FILE"] = trace_rel
    env["MC2_DIAG_TAGS"] = "CONFIG,BUILD,DEVICE,COMBAT"

    cmd = [sys.executable, os.path.join(root, "scripts", "run_smoke.py"),
           "--exe", args.exe, "--no-lease", "--mission", args.mission,
           "--duration", str(args.duration), "--keep-logs"]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    smoke_ok = (proc.returncode == 0)

    log = latest_artifact_log(root, args.mission)
    if not log:
        print("FAIL  brain_opord  artifact log not found (smoke_ok=%s)" % smoke_ok)
        return 1
    text = open(log, "r", encoding="utf-8", errors="replace").read()

    m = re.search(r"parsed \d+ Brain\{\} record\(s\): patrol=(\d+) guard=(\d+)", text)
    patrol_applied = int(m.group(1)) if m else 0
    guard_applied  = int(m.group(2)) if m else 0
    kicks  = text.count("[BRAIN_PATROL_KICK]")
    emits  = text.count("[BRAIN_ENGAGE] target=")

    hits = 0
    try:
        with open(jsonl, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if '"tag":"COMBAT"' in line and '"ev":"hit"' in line:
                    hits += 1
    except OSError:
        pass

    checks = [
        ("consumer parsed Brain block",       m is not None),
        ("patrol applied >= %d" % MIN_PATROL_APPLIED, patrol_applied >= MIN_PATROL_APPLIED),
        ("guard applied >= %d"  % MIN_GUARD_APPLIED,  guard_applied  >= MIN_GUARD_APPLIED),
        ("patrol kicks >= %d"   % MIN_PATROL_KICKS,   kicks  >= MIN_PATROL_KICKS),
        ("engage emits >= %d"   % MIN_ENGAGE_EMITS,   emits  >= MIN_ENGAGE_EMITS),
        ("weapon hits >= %d"    % MIN_HITS,           hits   >= MIN_HITS),
        ("smoke ok",                                  smoke_ok),
    ]
    passed = all(ok for _n, ok in checks)
    print("%s  brain_opord  %s  patrol=%d guard=%d kicks=%d emits=%d hits=%d smoke_ok=%s"
          % ("PASS" if passed else "FAIL", args.mission,
             patrol_applied, guard_applied, kicks, emits, hits, smoke_ok))
    if not passed:
        for n, ok in checks:
            if not ok:
                print("    MISS: %s" % n)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
