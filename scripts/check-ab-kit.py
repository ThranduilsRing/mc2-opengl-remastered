#!/usr/bin/env python3
"""Refute "each arm tests what its name says".

Parses the SHIPPED .bat files (not the generator's intent), replays the
`set` statements in order so LAST-WINS is honoured exactly as cmd.exe would,
then asserts the effective-environment delta against AB-0 equals precisely
the gate set named in the filename.

Also flags the two failure modes that produced this lane:
  * a blank `set VAR=` (deletes the variable, hands the gate to the JSON)
  * a gate named in the filename that the file does not actually set

Finally asserts the OTHER half of the read-me's claim -- that every arm shares
one BYTE-IDENTICAL common block. The effective-value replay above is blind to a
violation of that half: every gate appears TWICE in an arm (a common default
line, plus an override below for most arms), so a substitution that rewrites
BOTH copies leaves every effective value correct while the bytes diverge. That
is not hypothetical -- it is how an injected arm was once restored, and this
checker passed it; only an external sha256 manifest caught the difference.
"""
import difflib
import glob
import hashlib
import json
import os
import re
import sys

DST = sys.argv[1]

# The gates the engine still reads (Gates/gate_table.json). A kit arm that sets any other MC2_* name sets nothing: the gate was deleted or
# became an options.cfg entry, so the arm would silently A/B identical builds.
_TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Gates", "gate_table.json")
LIVE_GATES = {g["name"] for g in json.load(open(_TABLE, encoding="utf-8"))["gates"]}
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

# ---------------------------------------------------------------------------
# THE COMMON BLOCK IS BYTE-IDENTICAL -- checked, not asserted.
#
# The region is taken FROM THE FILES, not from the read-me's prose. Two
# anchors, both lines the arms actually contain, neither invented here:
#
#   START  `cd /d "%~dp0"` -- the first EXECUTABLE line, and unique in every
#          arm. Everything above it is that arm's own header docblock, which
#          legitimately differs (mission, traps, what a positive result is).
#   END    the arm's own banner line `REM  ####...`, which reads "THE ONLY
#          LINE(S) THIS ARM CHANGES FROM AB-0 -- BELOW HERE". Everything from
#          there down is the arm's override and prompt, which legitimately
#          differs. (AB-0 and AB-6a spell the same banner "#### This arm
#          overrides nothing. It IS the baseline. ####" -- same prefix.)
#
# So the region is exactly the intersection of "below the per-arm prose" and
# "above the per-arm override": the shared preamble, and nothing that any arm
# is entitled to change. As shipped that is 67 lines / 4108 bytes, identical
# across all nine.
#
# Compared as RAW BYTES including CRLF -- on a .bat a line-ending flip is a
# real difference, and normalising it away would reintroduce the blindness
# this check exists to remove.
# ---------------------------------------------------------------------------
CD_ANCHOR = b'cd /d "%~dp0"'
OVERRIDE_BANNER = b"REM  ####"
BLOCK_BANNER = b"REM  COMMON BLOCK -- byte-identical in every AB-*.bat in this folder."


def common_block(path):
    """Raw bytes of the shared preamble. Returns (block, error_or_None).

    Every failure to LOCATE the region is a hard error, never a silent skip:
    a checker that quietly passes a file it could not parse is the report-
    wearing-a-gate's-colour failure this kit already refuses elsewhere.
    """
    lines = open(path, "rb").read().split(b"\n")
    starts = [i for i, l in enumerate(lines) if l.rstrip(b"\r") == CD_ANCHOR]
    ends = [i for i, l in enumerate(lines) if l.startswith(OVERRIDE_BANNER)]
    if len(starts) != 1:
        return None, (f"expected exactly one `{CD_ANCHOR.decode()}` line, "
                      f"found {len(starts)}")
    if not ends:
        return None, ("no `REM  ####` override banner -- nothing separates the "
                      "shared preamble from this arm's own lines")
    if ends[0] < starts[0]:
        return None, "override banner appears ABOVE the `cd /d` anchor"
    block = b"\n".join(lines[starts[0]:ends[0]])
    if BLOCK_BANNER not in block:
        return None, "extracted region does not contain the COMMON BLOCK banner"
    return block, None


BLOCK_FAIL = 0
blocks = {}
for f in files:
    blk, err = common_block(f)
    if err:
        BLOCK_FAIL += 1
        print(f"FAIL {os.path.basename(f)}: common block not locatable -- {err}")
    else:
        blocks[f] = blk

ref = blocks.get(base_path)
if ref is None:
    BLOCK_FAIL += 1
    print("FAIL: no common block in the AB-0 baseline -- nothing to compare to")
else:
    ref_lines = ref.count(b"\n") + 1
    print(f"COMMON BLOCK sha256 {hashlib.sha256(ref).hexdigest()} "
          f"({len(ref)} bytes, {ref_lines} lines, "
          f"from {os.path.basename(base_path)})")
    ref_txt = [l.rstrip("\r") for l in ref.decode("ascii", "replace").split("\n")]
    for f in files:
        blk = blocks.get(f)
        if blk is None or blk == ref:
            continue
        BLOCK_FAIL += 1
        print(f"FAIL {os.path.basename(f)}: common block is NOT byte-identical "
              f"to {os.path.basename(base_path)} "
              f"(sha256 {hashlib.sha256(blk).hexdigest()}, {len(blk)} bytes)")
        blk_txt = [l.rstrip("\r") for l in blk.decode("ascii", "replace").split("\n")]
        if blk_txt == ref_txt:
            print("       text matches -- the difference is line endings or "
                  "trailing whitespace. Compare the raw bytes.")
        else:
            for line in difflib.unified_diff(
                    ref_txt, blk_txt, fromfile=os.path.basename(base_path),
                    tofile=os.path.basename(f), lineterm="", n=1):
                print("       " + line)

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
    # MC2_ prefix, e.g. "AB-3-CLOUD_SHADOW-1-..." claims MC2_CLOUD_SHADOW=1.
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
    retired = sorted(g for g in env if g not in LIVE_GATES)
    if retired:
        ok = False
        print(f"FAIL {name}: sets {retired}, which Gates/gate_table.json no longer lists (deleted or moved to options.cfg: the engine ignores them)")
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
        # documented bundle; the filename cannot carry several tokens. The ambient and PBR members of the original four became always-on
        # options.cfg entries (StaticPropAmbient, StaticPropPbr), so only the two env gates are left to set.
        expect = {"MC2_CLOUD_SHADOW", "MC2_WATER_SKYTINT"}
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
print(f"common block: {len(files) - BLOCK_FAIL}/{len(files)} arms byte-identical, "
      f"{BLOCK_FAIL} failing")
sys.exit(1 if (FAIL or BLOCK_FAIL) else 0)
