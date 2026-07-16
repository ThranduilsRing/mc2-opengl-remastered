#!/usr/bin/env python3
"""check-order-liveness.py — META-FIX design F runtime oracle (CLI-only).

Headless order-liveness gate: proves issued AI orders DO something. Runs the
MC2_FIXTURE_ORDER_LIVENESS fixture (code/mission.cpp) against a DEPLOYED exe
and asserts three legs (issues #17 reluctant fire / #10 scripted stalls /
#9 called-shot effectiveness):

  attack        (mc2_24) — every fixture-issued attack order yields >=1 COMBAT
                hit by that wid OR a classified `nofire` refusal stream OR the
                unit died (death exemption via the ev=disabled/destroyed wid);
                plus the global hits>=5 floor reused from check-combat-soak.
  move_stall    (mc2_16) — ORDER_LIVENESS_SUMMARY present + verdict=PASS, plus
                the frozen-signature classifier over the MC2_BRAIN_FREEZE_TRACE
                stderr stream: (a) ALARM-pinned + static + zero hits = FAIL;
                (b) movePath.numSteps>0 + static = FAIL; (c) brain-idle after
                holding a general order = WARN (until an mc2_16 baseline
                promotes it). EMPTY trace stream = exit 2 (harness error,
                never a silent pass — guards the oracle assumption that mc2_16
                has live scripted warriors).
  effectiveness (mc2_24) — with MC2_AIMED_FIRE_FIXTURE=1 + MC2_CALLED_SHOT_FIX=1
                every fixture hit is aimed-HEAD by construction; assert head
                share ("loc":0) of fixture-attacker hits >= 15% when n>=30
                (5%-clamp regression world passes < 2% of the time); n<30 =
                WARN not FAIL (stochastic leg).

LAUNCH MODEL: direct exe spawn (smoke-mode CLI: --profile/--mission/--duration)
NOT via run_smoke.py — the runner hard-caps --duration at 30 s and these legs
need ~90 s (Probe-E precedent, .claude/ISSUE-FIXES-STATUS.md). Own-PID
discipline: we only ever wait on / kill the child we spawned; concurrent
mc2.exe processes are never touched (NEVER --kill-existing semantics).

Timestamps: BRAIN_FREEZE_TRACE lines carry none, so the (a)/(b) windows are
approximated over the whole capture with a >=30-samples-per-wid basis
(conservative: flags full-run freezes; sub-window freezes need timestamps).

Usage:
  py -3 scripts/check-order-liveness.py --exe <deployed mc2.exe>
        [--duration 90] [--only attack] [--quiet] [--json out.json]

Exit: 0 = all legs pass, 1 = a gate failed, 2 = harness error
(check-combat-soak.py convention).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

EXIT_OK, EXIT_FAIL, EXIT_HARNESS = 0, 1, 2

ORDER_FIXTURE_RE = re.compile(
    r"\[ORDER_FIXTURE\] kind=(?P<kind>\w+) wid=(?P<wid>\d+) tgt=(?P<tgt>\d+)")
SUMMARY_RE = re.compile(
    r"ORDER_LIVENESS_SUMMARY\s+issued_move=(?P<im>\d+)\s+moved=(?P<mv>\d+)\s+"
    r"refused=(?P<rf>\d+)\s+stalled=(?P<st>\d+)\s+verdict=(?P<v>\w+)")
AIMED_ORDER_RE = re.compile(
    r"\[AIMED_FIRE v1\] event=order wid=(?P<wid>\d+)")
BRAIN_RE = re.compile(
    r"\[BRAIN_FREEZE_TRACE\] wid=(?P<wid>\d+) brainState=(?P<bs>-?\d+) "
    r"curTacOrder\.code=(?P<code>-?\d+) movePath\.numSteps=(?P<steps>-?\d+) "
    r"moveState=(?P<ms>-?\d+) hasBrain=(?P<hb>\d+) orderState=(?P<os>-?\d+) "
    r"mainGoal=(?P<mg>-?\d+) goalWID=(?P<gw>\d+) genOrder=(?P<go>-?\d+) "
    r"pos=\((?P<px>-?\d+),(?P<py>-?\d+)\) lastMoveErr=(?P<lme>-?\d+)")

MIN_SAMPLES = 30      # per-wid classifier basis (no timestamps in the stream)
STATIC_EPS_WU = 2.0   # world units; %.0f-printed pos => integer resolution


# ---------------------------------------------------------------------------
# Launch (direct spawn, own-PID discipline)
# ---------------------------------------------------------------------------

def launch(exe: str, mission: str, duration: int, env_extra: dict,
           trace_rel: str, quiet: bool):
    """Spawn the deployed exe in smoke mode; return (stdout, stderr,
    jsonl_path, exit_code) or None on harness error."""
    exe_dir = os.path.dirname(os.path.abspath(exe))
    jsonl_path = os.path.join(exe_dir, trace_rel)
    try:
        os.remove(jsonl_path)   # stale-proof (check-combat-soak.py precedent)
    except OSError:
        pass

    env = dict(os.environ)
    env.update({
        "MC2_SMOKE_MODE": "1",
        "MC2_LOG": "1",                 # engine redirects stdout->NUL otherwise
        "MC2_HEARTBEAT": "1",
        "MC2_DEBUG_STATE_DUMP": "1",
        "MC2_DIAGNOSTIC_TRACE_FILE": trace_rel,
        "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE,COMBAT",
        "MC2_SOAK_AUTOWIN": "1",
        "MC2_SOAK_WIN_AFTER_SEC": str(duration),
        "MC2_FIXTURE_ORDER_LIVENESS": "1",
    })
    env.update(env_extra)

    argv = [exe, "--profile", "stock", "--mission", mission,
            "--duration", str(duration + 15)]
    startupinfo = None
    if sys.platform == "win32":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 7   # SW_SHOWMINNOACTIVE: no focus steal
    if not quiet:
        print("  [launch] %s %s (%ds + grace)" % (mission,
              " ".join("%s=%s" % kv for kv in sorted(env_extra.items())),
              duration))
    proc = subprocess.Popen(argv, cwd=exe_dir, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            startupinfo=startupinfo)
    try:
        stdout, stderr = proc.communicate(timeout=duration + 15 + 90)
    except subprocess.TimeoutExpired:
        proc.kill()   # OWN child PID only — exact-PID discipline
        stdout, stderr = proc.communicate()
    return stdout or "", stderr or "", jsonl_path, proc.returncode


def read_combat_events(jsonl_path: str):
    """Return (hits, deaths_wids, nofire_wids, hit_by_awid) from the COMBAT
    JSONL; None if the trace file is missing (harness error)."""
    if not os.path.isfile(jsonl_path):
        return None
    hits = []
    death_wids: set[int] = set()
    nofire_wids: set[int] = set()
    with open(jsonl_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"tag":"COMBAT"' not in line:
                continue
            if '"ev":"hit"' in line:
                m_a = re.search(r'"a_wid":(\d+)', line)
                m_l = re.search(r'"loc":(-?\d+)', line)
                m_d = re.search(r'"dmg":([\d.]+)', line)
                hits.append({
                    "a_wid": int(m_a.group(1)) if m_a else -1,
                    "loc": int(m_l.group(1)) if m_l else -99,
                    "dmg": float(m_d.group(1)) if m_d else 0.0,
                })
            elif '"ev":"nofire"' in line:
                m = re.search(r'"wid":(\d+)', line)
                if m:
                    nofire_wids.add(int(m.group(1)))
            elif '"ev":"disabled"' in line or '"ev":"destroyed"' in line:
                m = re.search(r'"wid":(\d+)', line)
                if m:
                    death_wids.add(int(m.group(1)))
    return hits, death_wids, nofire_wids


# ---------------------------------------------------------------------------
# Legs
# ---------------------------------------------------------------------------

def leg_attack(exe: str, duration: int, quiet: bool) -> dict:
    r = launch(exe, "mc2_24", duration,
               {"MC2_FIRE_DECISION_TRACE": "1"},
               os.path.join("debug_state", "order_liveness_attack.jsonl"),
               quiet)
    stdout, stderr, jsonl, rc = r
    ev = read_combat_events(jsonl)
    if ev is None:
        return {"leg": "attack", "error": "trace file not found: %s" % jsonl,
                "passed": False, "harness": True}
    hits, death_wids, nofire_wids = ev
    ordered = [int(m.group("wid")) for m in ORDER_FIXTURE_RE.finditer(stdout)
               if m.group("kind") == "attack"]
    if not ordered:
        return {"leg": "attack",
                "error": "no [ORDER_FIXTURE] kind=attack lines — fixture "
                         "never issued (MC2_SOAK_AUTOWIN window too short, or "
                         "gate dropped?)",
                "passed": False, "harness": True}
    hit_wids = {h["a_wid"] for h in hits}
    dead_ends = [w for w in set(ordered)
                 if w not in hit_wids and w not in nofire_wids
                 and w not in death_wids]
    total_hits = len(hits)
    passed = (not dead_ends) and (total_hits >= 5)
    return {"leg": "attack", "ordered": len(set(ordered)),
            "hits": total_hits, "nofire_wids": len(nofire_wids),
            "deaths": len(death_wids), "dead_end_wids": dead_ends,
            "min_hits": 5, "exit_code": rc, "passed": passed}


def classify_brain_stream(stderr_text: str, hit_wids: set[int]):
    """Fixture-C frozen-signature classifier. Returns (fails, warns,
    n_lines). Whole-capture approximation (no timestamps; see module doc)."""
    per_wid: dict[int, list] = {}
    n = 0
    for m in BRAIN_RE.finditer(stderr_text):
        n += 1
        per_wid.setdefault(int(m.group("wid")), []).append((
            int(m.group("os")), int(m.group("steps")),
            float(m.group("px")), float(m.group("py")),
            int(m.group("go")), int(m.group("code"))))
    fails, warns = [], []
    for wid, samples in sorted(per_wid.items()):
        if len(samples) < MIN_SAMPLES:
            continue
        xs = [s[2] for s in samples]; ys = [s[3] for s in samples]
        static = (max(xs) - min(xs) <= STATIC_EPS_WU and
                  max(ys) - min(ys) <= STATIC_EPS_WU)
        # (a) ALARM-pinned + static + zero hits -> FAIL
        if static and all(s[0] == 2 for s in samples) and wid not in hit_wids:
            fails.append("wid=%d frozen(a): ALARM-pinned all %d samples, "
                         "static, zero hits" % (wid, len(samples)))
        # (b) path-but-not-moving -> FAIL
        steps_pos = sum(1 for s in samples if s[1] > 0)
        if static and steps_pos >= max(MIN_SAMPLES, int(0.75 * len(samples))):
            fails.append("wid=%d frozen(b): movePath.numSteps>0 in %d/%d "
                         "samples but position static" %
                         (wid, steps_pos, len(samples)))
        # (c) brain idle after holding a general order -> WARN (baseline
        # pending; scripted patrols that legitimately complete idle forever)
        held = any(s[4] != 0 for s in samples)
        tail = samples[-MIN_SAMPLES:]
        if held and all(s[4] == 0 and s[5] == 0 for s in tail):
            warns.append("wid=%d frozen(c) WARN: general order held then "
                         "brain idle for last %d samples" % (wid, len(tail)))
    return fails, warns, n


def leg_move_stall(exe: str, duration: int, quiet: bool) -> dict:
    r = launch(exe, "mc2_16", duration,
               {"MC2_BRAIN_FREEZE_TRACE": "1"},
               os.path.join("debug_state", "order_liveness_move.jsonl"),
               quiet)
    stdout, stderr, jsonl, rc = r
    ev = read_combat_events(jsonl)
    hit_wids = {h["a_wid"] for h in ev[0]} if ev else set()

    m = SUMMARY_RE.search(stdout)
    fails, warns = [], []
    if not m:
        fails.append("ORDER_LIVENESS_SUMMARY absent (fixture died silently "
                     "or never reached 0.9*win)")
        summary = None
    else:
        summary = {k: m.group(g) for k, g in
                   (("issued_move", "im"), ("moved", "mv"), ("refused", "rf"),
                    ("stalled", "st"), ("verdict", "v"))}
        if m.group("v") != "PASS":
            fails.append("summary verdict=%s (issued=%s moved=%s refused=%s "
                         "stalled=%s)" % (m.group("v"), m.group("im"),
                                          m.group("mv"), m.group("rf"),
                                          m.group("st")))

    brain_fails, brain_warns, n_lines = classify_brain_stream(stderr, hit_wids)
    if n_lines == 0:
        # Empty stream = harness error, NEVER a silent pass (guards the
        # unverified 'mc2_16 has live scripted warriors' oracle assumption).
        return {"leg": "move_stall",
                "error": "MC2_BRAIN_FREEZE_TRACE stream is EMPTY — oracle "
                         "assumption violated or stderr capture broken",
                "passed": False, "harness": True}
    fails.extend(brain_fails)
    warns.extend(brain_warns)
    return {"leg": "move_stall", "summary": summary,
            "brain_lines": n_lines, "fails": fails, "warns": warns,
            "exit_code": rc, "passed": not fails}


def leg_effectiveness(exe: str, duration: int, quiet: bool) -> dict:
    # NOTE: the order-liveness fixture is explicitly DISABLED here ("0" =
    # unset to the engine latch) — its unaimed 0.3*win attack orders would
    # install combat orders that block the aimed fixture's re-issue loop
    # (!curTacOrder.isCombatOrder()) and dilute the aimed-HEAD hit stream.
    r = launch(exe, "mc2_24", duration,
               {"MC2_AIMED_FIRE_FIXTURE": "1", "MC2_CALLED_SHOT_FIX": "1",
                "MC2_FIXTURE_ORDER_LIVENESS": "0"},
               os.path.join("debug_state", "order_liveness_aimed.jsonl"),
               quiet)
    stdout, stderr, jsonl, rc = r
    ev = read_combat_events(jsonl)
    if ev is None:
        return {"leg": "effectiveness", "error": "trace file not found: %s"
                % jsonl, "passed": False, "harness": True}
    hits, _deaths, _nofire = ev
    fixture_wids = {int(m.group("wid"))
                    for m in AIMED_ORDER_RE.finditer(stdout)}
    if not fixture_wids:
        return {"leg": "effectiveness",
                "error": "no [AIMED_FIRE v1] event=order lines — aimed "
                         "fixture never issued", "passed": False,
                "harness": True}
    fx_hits = [h for h in hits if h["a_wid"] in fixture_wids]
    n = len(fx_hits)
    head = sum(1 for h in fx_hits if h["loc"] == 0)
    share = (head / n) if n else 0.0
    if n < 30:
        return {"leg": "effectiveness", "n_hits": n, "head": head,
                "head_share": round(share, 3), "warn":
                "n<30 fixture hits — stochastic leg degraded to WARN "
                "(needs a longer window)", "exit_code": rc, "passed": True}
    passed = share >= 0.15
    return {"leg": "effectiveness", "n_hits": n, "head": head,
            "head_share": round(share, 3), "floor": 0.15,
            "exit_code": rc, "passed": passed}


LEGS = {
    "attack": leg_attack,
    "move_stall": leg_move_stall,
    "effectiveness": leg_effectiveness,
}


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Order-liveness runtime gate (META-FIX design F)")
    ap.add_argument("--exe", required=True, help="Deployed mc2.exe")
    ap.add_argument("--duration", type=int, default=90,
                    help="Autowin window per leg in seconds (default 90; the "
                         "exe self-exits at duration+15)")
    ap.add_argument("--only", action="append", default=None,
                    help="Restrict to leg(s): attack / move_stall / "
                         "effectiveness; repeatable")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print("FAIL  order_liveness  exe not found: %s" % args.exe)
        return EXIT_HARNESS

    names = list(LEGS)
    if args.only:
        names = [n for n in names if n in args.only]
        if not names:
            print("FAIL  order_liveness  no leg matched --only %s" % args.only)
            return EXIT_HARNESS

    results, harness_err = [], False
    for name in names:
        r = LEGS[name](args.exe, args.duration, args.quiet)
        results.append(r)
        if r.get("harness"):
            harness_err = True
        if not args.quiet:
            if "error" in r:
                print("FAIL  order_liveness  %-13s  %s" % (name, r["error"]))
            else:
                verdict = "PASS" if r["passed"] else "FAIL"
                detail = {k: v for k, v in r.items()
                          if k not in ("leg", "passed")}
                print("%s  order_liveness  %-13s  %s" % (verdict, name, detail))
                for w in r.get("warns", []) or []:
                    print("      WARN  %s" % w)

    n_fail = sum(1 for r in results if not r.get("passed"))
    if not args.quiet:
        print("\norder_liveness: %d leg(s), %d passed, %d failed"
              % (len(results), len(results) - n_fail, n_fail))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as jf:
            json.dump({"result": "FAIL" if n_fail else "PASS",
                       "legs": results}, jf, indent=2)
    if harness_err:
        return EXIT_HARNESS
    return EXIT_FAIL if n_fail else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
