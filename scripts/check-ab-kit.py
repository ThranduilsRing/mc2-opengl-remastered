#!/usr/bin/env python3
"""Refute "each arm tests what its name says".

Parses the SHIPPED .bat files (not the generator's intent), replays the
`set` statements in order so LAST-WINS is honoured exactly as cmd.exe would,
then asserts the effective-environment delta against AB-0 equals precisely
the gate set named in the filename.

Also flags the two failure modes that produced this lane:
  * a blank `set VAR=` (deletes the variable, hands the gate to the JSON)
  * a gate named in the filename that the file does not actually set
"""
import glob
import os
import re
import sys

DST = sys.argv[1]
SET_RE = re.compile(r"^set\s+(MC2_[A-Z0-9_]+)=(.*)$", re.IGNORECASE)

# Keys launcher_env.json can actually reach. A blank `set` is banned ONLY for
# these -- derived from the shipped file, not from a blanket policy, because a
# blank IS the correct "off" for a presence-test variable the JSON never names.
JSON_PATH = os.path.join(DST, "launcher_env.json")
JSON_KEYS = set(re.findall(r'"(MC2_[A-Z0-9_]+)"\s*:\s*"[^"]', open(JSON_PATH).read())) \
    if os.path.exists(JSON_PATH) else set()

# Presence-test readers: `=0` ENABLES, so a blank delete is REQUIRED.
PRESENCE_TEST = {"MC2_TREE_DIAG_TRACE", "MC2_LOG"}
# Instruments, not rendering gates: declared deltas that do not change a pixel.
INSTRUMENTS = {"MC2_LOG"}

def effective(path):
    env = {}
    blanks = []
    for raw in open(path, encoding="ascii", errors="replace"):
        line = raw.strip()
        if line.upper().startswith("REM"):
            continue
        m = SET_RE.match(line)
        if not m:
            continue
        var, val = m.group(1), m.group(2).strip()
        if val == "":
            blanks.append(var)
            env.pop(var, None)          # cmd.exe semantics: blank DELETES
        else:
            env[var] = val
    return env, blanks

files = sorted(glob.glob(os.path.join(DST, "AB-*.bat")))
base_path = [f for f in files if os.path.basename(f).startswith("AB-0-")][0]
base, base_blanks = effective(base_path)

print(f"BASELINE {os.path.basename(base_path)}: {len(base)} gates set, "
      f"{len(base_blanks)} deliberate deletions")
if base_blanks:
    unexpected = [b for b in base_blanks if b not in PRESENCE_TEST]
    print(f"  presence-test vars deleted (correct, `=0` would ENABLE them): "
          f"{sorted(b for b in base_blanks if b in PRESENCE_TEST)}")
    if unexpected:
        print(f"  !! UNEXPECTED blank set: {unexpected}")
print(f"  launcher_env.json keys a blank would hand back: {len(JSON_KEYS)}")

FAIL = 0
for f in files:
    name = os.path.basename(f)
    env, blanks = effective(f)
    delta = {k: (base.get(k), v) for k, v in env.items() if base.get(k) != v}
    delta.update({k: (v, None) for k, v in base.items() if k not in env})
    instrument_delta = {k: v for k, v in delta.items() if k in INSTRUMENTS}
    for k in instrument_delta:
        del delta[k]        # reported separately; not a rendering claim

    # What does the FILENAME claim? Gate tokens appear as MC2_<NAME> minus the
    # MC2_ prefix, e.g. "AB-3-SUN_AXIS_FIX-1-..." claims MC2_SUN_AXIS_FIX=1.
    claimed = {}
    for gate in base:
        tok = gate[len("MC2_"):]
        m = re.search(re.escape(tok) + r"-([01](?:\.\d+)?)\b", name)
        if m:
            claimed[gate] = m.group(1)

    ok = True
    # A filename token asserts the EFFECTIVE VALUE of that gate in that file
    # (not merely a delta) -- so an arm that deliberately sits at the default
    # while adding instrumentation still has to name the value it runs at.
    for g, want in claimed.items():
        if env.get(g) != want:
            ok = False
            print(f"FAIL {name}: filename says {g}={want}, file runs at "
                  f"{env.get(g)!r}")
    undeclared = set(delta) - set(claimed)
    if undeclared and not name.startswith(("AB-0-", "AB-7-")):
        ok = False
        print(f"FAIL {name}: changes gate(s) its filename does not name: "
              f"{sorted(undeclared)}")

    if name.startswith("AB-0-"):
        if delta:
            ok = False
            print(f"FAIL {name}: baseline must have no delta, got {delta}")
    elif name.startswith("AB-7-"):
        # documented 4-gate bundle; filename cannot carry four tokens
        expect = {"MC2_STATIC_PROP_AMBIENT_V1", "MC2_STATIC_PROP_PBR_V1",
                  "MC2_CLOUD_SHADOW", "MC2_WATER_SKYTINT"}
        if set(delta) != expect or any(v[1] != "1" for v in delta.values()):
            ok = False
            print(f"FAIL {name}: expected exactly {sorted(expect)} ->1, got {delta}")
    else:
        if not claimed:
            ok = False
            print(f"FAIL {name}: filename names no gate this file sets")
    bad_blanks = [b for b in blanks if b in JSON_KEYS]
    if bad_blanks:
        ok = False
        print(f"FAIL {name}: BLANK `set VAR=` deletes {bad_blanks}, which "
              f"launcher_env.json DOES define -> hands the gate back to it")
    missing_pt = [p for p in PRESENCE_TEST if p not in blanks and p not in env]
    if missing_pt:
        ok = False
        print(f"FAIL {name}: presence-test var(s) {missing_pt} neither deleted "
              f"nor set -- a leftover in the user's shell would leak in")
    for p in PRESENCE_TEST:
        if env.get(p) == "0":
            ok = False
            print(f"FAIL {name}: {p}=0 ENABLES it (presence test); use a blank set")
    if "MC2_LAUNCHER_ENV_JSON" not in env or env["MC2_LAUNCHER_ENV_JSON"] != "0":
        ok = False
        print(f"FAIL {name}: MC2_LAUNCHER_ENV_JSON is not exactly 0 -- the engine "
              f"will apply launcher_env.json to every unset MC2_*")
    if not ok:
        FAIL += 1
    else:
        d = ", ".join(f"{k}: {v[0]}->{v[1]}" for k, v in sorted(delta.items())) or "(none)"
        i = ", ".join(f"{k}: {v[0]}->{v[1]}" for k, v in sorted(instrument_delta.items()))
        print(f"PASS {name}\n       gate delta vs AB-0: {d}"
              + (f"\n       instrument (no pixels): {i}" if i else ""))

print()
print(f"{len(files)} arms checked, {FAIL} failing")
sys.exit(1 if FAIL else 0)
