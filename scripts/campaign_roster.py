#!/usr/bin/env python3
"""Expected mech roster per mission, and the per-mech state against what a run saw.

Usage:
  py -3 scripts/campaign_roster.py expected <mission.fit> [--objects-csv art/buildings.csv ...] [--all-movers]
  py -3 scripts/campaign_roster.py compare  <mission.fit> <stdout.txt> [--captures captures.json]
                                           [--objects-csv ...] [--mission NAME] [--home-team 0]

Source of truth is the mission .fit ([PartN] blocks) plus the object table (buildings.csv
Type column: MECH / VEHICLE) -- never a hand-kept list. The ABL script is scanned for
reinforcement triggers (late spawns); parts the fit marks Active=0 or Exists=0 are late too.

Three states per expected mech, taken from the [MOVER v2] samples of the run (first live
sighting) and the optional per-mech capture manifest (scripts/mech_capture_check.py):
  passed      seen live, and its capture (when captures were taken) passed the pixel check
  failed      seen live, but the capture failed the pixel check or is missing
  never_seen  no live mover of that name/team ever appeared (late spawn that never fired
              shows up here with late=true)
Player-team slots match by team only: the player's lance comes from the bay, not the fit.

Pure text in, dict out. Unit-tested from saved data (scripts/tests/data).
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import campaign_liveness as cl  # noqa: E402

PART_SPLIT = re.compile(r"^\[Part(\d+)\]\s*$", re.M)
KEY_RE = re.compile(r"^\w+\s+(\w+)\s*=\s*(.*?)\s*$")
REINFORCE_RE = re.compile(r"reinforce|spawn|callin|call_in|arrive", re.I)


def _val(s):
    s = s.strip()
    return s[1:-1] if len(s) >= 2 and s[0] == '"' and s[-1] == '"' else s


def parse_fit_parts(text):
    """Mission .fit text -> list of part dicts (every [PartN] block)."""
    parts = []
    pieces = PART_SPLIT.split(text)
    for i in range(1, len(pieces), 2):
        body = pieces[i + 1]
        body = re.split(r"^\[", body, maxsplit=1, flags=re.M)[0]
        kv = {}
        for line in body.splitlines():
            m = KEY_RE.match(line.strip())
            if m:
                kv[m.group(1)] = _val(m.group(2))
        parts.append({
            "part": int(pieces[i]), "name": kv.get("CSVFile", "?"), "profile": kv.get("ObjectProfile", ""),
            "object": int(kv.get("ObjectNumber", "-1") or -1), "team": int(kv.get("TeamID", "-1") or -1),
            "player": kv.get("PlayerPart", "FALSE").upper() == "TRUE",
            "active": kv.get("Active", "1") != "0", "exists": kv.get("Exists", "1") != "0",
            "pos": (float(kv.get("PositionX", "0") or 0), float(kv.get("PositionY", "0") or 0)),
        })
    return parts


def load_object_types(csv_paths):
    """buildings.csv rows -> {lowercase file name: (Type, SPECIAL TYPE)}; later paths override earlier ones."""
    types = {}
    for p in csv_paths:
        lines = Path(p).read_text(errors="replace").splitlines()
        if not lines:
            continue
        head = [h.strip() for h in lines[0].split(",")]
        try:
            it, isp = head.index("Type"), head.index("SPECIAL TYPE")
        except ValueError:
            continue
        for line in lines[1:]:
            c = line.split(",")
            if len(c) > max(it, isp) and c[0].strip():
                types[c[0].strip().lower()] = (c[it].strip(), c[isp].strip())
    return types


def classify(part, types):
    """'mech' | 'vehicle' | 'other'. Falls back to the PM... profile prefix when the object is not in the table."""
    t = types.get(part["name"].lower()) if types else None
    if t:
        return "mech" if t[0].upper() == "MECH" else "vehicle" if t[0].upper() == "VEHICLE" else "other"
    prof = part["profile"].lower()
    return "mech" if prof.startswith("pm") else "vehicle" if prof.startswith("pv") else "other"


def abl_late_triggers(abl_text):
    """Names of ABL declarations that look like reinforcement / late-spawn triggers."""
    seen = []
    for line in abl_text.splitlines():
        code = line.split("//", 1)[0]
        if REINFORCE_RE.search(code) and re.search(r"\b(boolean|integer|real|char)\b", code, re.I):
            m = re.search(r"\b(\w*(?:reinforce|spawn|callin|arrive)\w*)\s*;", code, re.I)
            if m and m.group(1) not in seen:
                seen.append(m.group(1))
    return seen


def expected_roster(fit_text, types=None, abl_text="", movers=False):
    """-> {mechs:[...], late_triggers:[...]}; every entry is one fit part. movers=True keeps vehicles too."""
    out = []
    for p in parse_fit_parts(fit_text):
        kind = classify(p, types)
        if kind == "other" or (kind == "vehicle" and not movers):
            continue
        sp = (types or {}).get(p["name"].lower(), ("", ""))[1]
        out.append({**p, "kind": kind, "special": sp, "late": (not p["active"]) or (not p["exists"])})
    return {"mechs": out, "late_triggers": abl_late_triggers(abl_text) if abl_text else []}


def _observed(segment):
    """segment -> {id: {name, team, first_t, kind_seen}} (every distinct mover id ever sampled)."""
    seen = {}
    for s in segment["samples"]:
        for m in s["movers"]:
            seen.setdefault(m["id"], {"id": m["id"], "name": m["name"], "team": m["team"], "first_t": s["t"]})
    return seen


def compare(roster, segment, captures=None, home_team=0):
    """Per expected entry: passed / failed / never_seen.

    captures: list of {id, name, team, passed} from the per-mech capture manifest, or None when no
    captures were taken (then 'passed' means 'seen live'). Matching is greedy per (name, team); player
    slots match any team-home mech.
    """
    obs = _observed(segment)
    cap_by_id = {c["id"]: c for c in (captures or [])}
    used = set()
    rows = []
    for e in sorted(roster["mechs"], key=lambda r: (r["late"], r["part"])):
        is_player = e["team"] == home_team
        cand = [o for o in sorted(obs.values(), key=lambda o: (o["first_t"], o["id"]))
                if o["id"] not in used and o["team"] == e["team"] and (is_player or o["name"].replace("_", " ").lower() == e["name"].lower().replace("_", " "))]
        # engine mover names have spaces swapped for '_' in the log; the sanitised forms compare equal above
        hit = cand[0] if cand else None
        row = {"part": e["part"], "name": e["name"], "team": e["team"], "late": e["late"], "kind": e["kind"], "special": e["special"]}
        if hit is None:
            row.update(state="never_seen", id=None, first_t=None)
        else:
            used.add(hit["id"])
            cap = cap_by_id.get(hit["id"])
            if captures is None:
                state = "passed"
            elif cap is None or not cap.get("passed"):
                state = "failed"
            else:
                state = "passed"
            row.update(state=state, id=hit["id"], first_t=hit["first_t"], seen_as=hit["name"],
                       capture=None if cap is None else cap.get("png"))
        rows.append(row)
    extra = [o for o in obs.values() if o["id"] not in used]
    counts = {s: sum(r["state"] == s for r in rows) for s in ("passed", "failed", "never_seen")}
    return {"rows": rows, "counts": counts, "unexpected_movers": len(extra),
            "late_triggers": roster["late_triggers"]}


def main(argv):
    if len(argv) < 2 or argv[0] not in ("expected", "compare"):
        print(__doc__)
        return 2
    args = list(argv)
    csvs, captures, mission, home, movers = [], None, None, 0, False
    for flag in ("--objects-csv", "--captures", "--mission", "--home-team"):
        while flag in args:
            i = args.index(flag)
            v = args[i + 1]
            del args[i:i + 2]
            if flag == "--objects-csv":
                csvs.append(v)
            elif flag == "--captures":
                captures = json.loads(Path(v).read_text())
            elif flag == "--mission":
                mission = v
            else:
                home = int(v)
    if "--all-movers" in args:
        args.remove("--all-movers")
        movers = True
    fit = Path(args[1])
    abl = fit.with_suffix(".abl")
    roster = expected_roster(fit.read_text(errors="replace"), load_object_types(csvs),
                             abl.read_text(errors="replace") if abl.exists() else "", movers)
    if args[0] == "expected":
        print(json.dumps(roster, indent=2))
        return 0
    segs = [s for s in cl.parse_log(Path(args[2]).read_text(errors="replace")) if s["samples"]]
    seg = next((s for s in segs if mission and s["mission"] == mission), segs[0] if segs else {"samples": []})
    res = compare(roster, seg, captures, home)
    print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
