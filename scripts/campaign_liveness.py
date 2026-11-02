#!/usr/bin/env python3
"""Campaign liveness: parse [MOVER v2] samples from a soak/sweep stdout log.

Usage: py -3 scripts/campaign_liveness.py <stdout.txt> [--home-team 0] [--json OUT]

Input is the stdout of a run with MC2_BRIDGE_MOVER_STATE=1 (see docs/tier1_env_vars.md).
Per mission segment it answers, for the non-player movers: did they move, is their AI
doing anything, and is anything invisible / at hp ~0 from the very first sample.
Missions are split where scenario time goes backwards; a `[SOAK] autowin mission=X`
line names the segment it ends (the win is emitted after that mission's samples).

Pure text in, dict out: no game, no host lease. Unit-tested from a saved real log
(scripts/tests/data/mover_v2_sample.txt) so it never depends on a game run.
"""
import json
import math
import re
import sys
from pathlib import Path

BEGIN_RE = re.compile(r"^\[MOVER v2 begin\] t=(?P<t>[-\d.]+) count=(?P<count>\d+)")
LINE_RE = re.compile(r"^\[MOVER v2\] (?P<kv>.*)$")
AUTOWIN_RE = re.compile(r"^\[SOAK\] autowin mission=(?P<m>\S+)")
KV_RE = re.compile(r"(\w+)=(\S+)")

ENGAGE_DIST = 400.0   # metres; closer than this and idle enemies are a real AI failure
MOVE_THRESHOLD = 20.0     # world units; a guard shuffling in place does not count as movement
HP_NEAR_ZERO = 0.05


def _xyz(s):
    try:
        return tuple(float(v) for v in s.split(","))
    except ValueError:
        return (0.0, 0.0, 0.0)


def parse_mover_line(line):
    """One `[MOVER v2] ...` line -> dict, or None if it is not one."""
    m = LINE_RE.match(line.strip())
    if not m:
        return None
    kv = dict(KV_RE.findall(m.group("kv")))
    try:
        order_val, _, order_name = kv.get("order", "-1/none").partition("/")
        pstep, _, psteps = kv.get("pstep", "-1/0").partition("/")
        return {
            "id": int(kv["id"]), "name": kv.get("name", "?"), "team": int(kv["team"]),
            "pos": _xyz(kv.get("pos", "0,0,0")), "hp": float(kv.get("hp", "1")),
            "pilot": kv.get("pilot", "?"), "order": int(order_val), "order_name": order_name or "none",
            "target": int(kv.get("target", "-1")), "brain": int(kv.get("brain", "-1")),
            "mstate": int(kv.get("mstate", "-1")), "pstep": int(pstep), "psteps": int(psteps),
            "pcost": int(kv.get("pcost", "-1")),
        }
    except (KeyError, ValueError):
        return None


def parse_log(text):
    """Log text -> list of mission segments: {mission, samples:[{t,count,movers:[...]}]}."""
    segments = []
    cur = None
    sample = None
    last_t = None
    for raw in text.splitlines():
        line = raw.strip()
        b = BEGIN_RE.match(line)
        if b:
            t = float(b.group("t"))
            if cur is None or (last_t is not None and t < last_t - 1.0):
                cur = {"mission": None, "samples": []}
                segments.append(cur)
            last_t = t
            sample = {"t": t, "count": int(b.group("count")), "movers": []}
            cur["samples"].append(sample)
            continue
        mv = parse_mover_line(line)
        if mv is not None and sample is not None:
            sample["movers"].append(mv)
            continue
        w = AUTOWIN_RE.match(line)
        if w and cur is not None and cur["mission"] is None:
            cur["mission"] = w.group("m")
            cur = None          # the next sample opens a fresh segment
            last_t = None
    return segments


def _dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def analyse_segment(seg, home_team=0):
    """Per-mover tracks and the mission verdict for one segment."""
    tracks = {}
    for s in seg["samples"]:
        for m in s["movers"]:
            k = (m["id"], m["name"], m["team"])
            tr = tracks.setdefault(k, {"id": m["id"], "name": m["name"], "team": m["team"], "first_t": s["t"],
                                       "first_pos": m["pos"], "first_hp": m["hp"], "max_disp": 0.0,
                                       "min_hp": m["hp"], "last_hp": m["hp"], "last_t": s["t"],
                                       "targeted": False, "path": False, "brain_states": set(),
                                       "mstates": set(), "orders": set(), "samples": 0})
            tr["last_t"] = s["t"]
            tr["samples"] += 1
            tr["max_disp"] = max(tr["max_disp"], _dist(m["pos"], tr["first_pos"]))
            tr["min_hp"] = min(tr["min_hp"], m["hp"])
            tr["last_hp"] = m["hp"]
            tr["targeted"] |= m["target"] != -1
            tr["path"] |= m["psteps"] > 0
            tr["brain_states"].add(m["brain"])
            tr["mstates"].add(m["mstate"])
            tr["orders"].add(m["order_name"])
    out = []
    for tr in tracks.values():
        enemy = tr["team"] != home_team
        moved = tr["max_disp"] >= MOVE_THRESHOLD
        ai_on = moved or tr["targeted"] or tr["path"] or len(tr["brain_states"]) > 1 or len(tr["mstates"]) > 1
        out.append({
            "id": tr["id"], "name": tr["name"], "team": tr["team"], "enemy": enemy,
            "samples": tr["samples"], "first_t": tr["first_t"], "last_t": tr["last_t"],
            "max_disp": round(tr["max_disp"], 1), "moved": moved, "ai_on": ai_on,
            "first_hp": tr["first_hp"], "min_hp": tr["min_hp"],
            "hp_near_zero_at_start": tr["first_hp"] <= HP_NEAR_ZERO,
            "targeted": tr["targeted"], "orders": sorted(tr["orders"]),
        })
    # Closest approach of any home mover to any enemy in the same sample: an enemy that never saw the player
    # (scripted dormant until triggered) is not an AI failure.
    min_d = None
    for smp in seg["samples"]:
        hm = [m["pos"] for m in smp["movers"] if m["team"] == home_team]
        em = [m["pos"] for m in smp["movers"] if m["team"] != home_team]
        for h in hm:
            for e in em:
                d = _dist(h, e)
                min_d = d if min_d is None else min(min_d, d)
    enemies = [t for t in out if t["enemy"]]
    sustained = [t for t in enemies if t["last_t"] - t["first_t"] >= 10.0]
    summary = {
        "mission": seg["mission"], "samples": len(seg["samples"]),
        "t_first": seg["samples"][0]["t"] if seg["samples"] else None,
        "t_last": seg["samples"][-1]["t"] if seg["samples"] else None,
        "movers": len(out), "enemies": len(enemies),
        "enemies_moved": sum(t["moved"] for t in enemies),
        "enemies_ai_on": sum(t["ai_on"] for t in enemies),
        "min_home_enemy_dist": None if min_d is None else round(min_d, 1),
        "enemies_hp_near_zero_at_start": [t["id"] for t in enemies if t["hp_near_zero_at_start"]],
    }
    # Verdict: needs a real observation window, at least one enemy acting, none dead on arrival.
    if not enemies:
        summary["verdict"] = "no_enemies"
    elif not sustained:
        summary["verdict"] = "window_too_short"
    elif summary["enemies_hp_near_zero_at_start"]:
        summary["verdict"] = "fail_hp_near_zero"
    elif summary["enemies_ai_on"] == 0 and (min_d is None or min_d > ENGAGE_DIST):
        summary["verdict"] = "dormant_unprovoked"   # nobody came within ENGAGE_DIST: scripted-idle, not a failure
    elif summary["enemies_ai_on"] == 0:
        summary["verdict"] = "fail_no_ai"
    else:
        summary["verdict"] = "pass"
    return {"summary": summary, "movers": out}


def analyse(text, home_team=0):
    return [analyse_segment(s, home_team) for s in parse_log(text) if s["samples"]]


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    home = 0
    out = None
    args = list(argv)
    if "--home-team" in args:
        i = args.index("--home-team")
        home = int(args[i + 1])
        del args[i:i + 2]
    if "--json" in args:
        i = args.index("--json")
        out = args[i + 1]
        del args[i:i + 2]
    res = analyse(Path(args[0]).read_text(errors="replace"), home)
    for r in res:
        s = r["summary"]
        print(f"{s['mission'] or '?':12s} {s['verdict']:20s} samples={s['samples']} t={s['t_first']}..{s['t_last']} "
              f"enemies={s['enemies']} moved={s['enemies_moved']} ai_on={s['enemies_ai_on']}")
    if out:
        Path(out).write_text(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
