import re, glob, os, collections, json, sys
ROOT = "A:/Games/mc2-main/tests/smoke/artifacts"
RX = re.compile(r"\[VIS-SEM v1\] cls=(\w+) actor=(\w+) frame=(\d+) sensorLevel=(-?\d+) meshAlpha=(\d+) status=(-?\d+) relation=(-?\d+) legacyVisible=(\d)")
DESTROYED, DISABLED, SHUTDOWN = 3, 4, 5   # placeholder; resolved below

# resolve OBJECT_STATUS_* from objstatus.h
st = open("A:/Games/mc2-main/mclib/objstatus.h").read()
vals = {}
n = 0
for line in st.splitlines():
    m = re.match(r"\s*(OBJECT_STATUS_\w+)\s*(?:=\s*(\d+))?\s*,?", line)
    if m and "enum" not in line:
        if m.group(2) is not None: n = int(m.group(2))
        vals[m.group(1)] = n; n += 1
DESTROYED = vals.get("OBJECT_STATUS_DESTROYED", 3)
DISABLED  = vals.get("OBJECT_STATUS_DISABLED", 4)
SHUTDOWN  = vals.get("OBJECT_STATUS_SHUTDOWN", 5)
print("status codes:", {k:v for k,v in vals.items() if k.split('_')[-1] in ("NORMAL","DESTROYED","DISABLED","SHUTDOWN")}, file=sys.stderr)

def classify(sl, alpha, rel, status, legacy):
    powered = status not in (DESTROYED, DISABLED, SHUTDOWN)
    friendly = rel in (0,1)
    meshDrawn_new = True if friendly else (alpha != 0)
    emits_new     = powered and (friendly or alpha != 0)
    emits_armed   = powered and bool(legacy) and (sl == 0 or sl >= 5)
    emits_parked  = powered           # shipped binaries: mover_model_shown -> true
    return powered, friendly, meshDrawn_new, emits_new, emits_armed, emits_parked

CATS = ["player_own(rel0)","allied(rel1)","enemy_full_contact","enemy_blip_only",
        "enemy_hidden(alpha0,sl0)","destroyed/disabled/shutdown"]

def cat(sl, alpha, rel, status):
    if status in (DESTROYED, DISABLED, SHUTDOWN): return CATS[5]
    if rel == 0: return CATS[0]
    if rel == 1: return CATS[1]
    if 1 <= sl <= 4: return CATS[3]
    if alpha == 0: return CATS[4]
    return CATS[2]

out = {}
for d in sorted(glob.glob(os.path.join(ROOT, "2026-07-25T15-*"))):
    for f in glob.glob(os.path.join(d, "*.log")):
        stem = os.path.basename(f).split(".")[0]
        if not stem.startswith("mc2_"): continue
        rows = []
        for line in open(f, errors="ignore"):
            m = RX.search(line)
            if m: rows.append(m.groups())
        if not rows: continue
        agg = out.setdefault(stem, collections.defaultdict(lambda: collections.Counter()))
        for cls, actor, frame, sl, alpha, status, rel, legacy in rows:
            sl, alpha, status, rel, legacy = int(sl), int(alpha), int(status), int(rel), int(legacy)
            c = cat(sl, alpha, rel, status)
            p, fr, md, en, ea, ep = classify(sl, alpha, rel, status, legacy)
            k = agg[(cls, c)]
            k["samples"] += 1
            k["emits_NEW"] += en
            k["emits_ARMED_LEGACY"] += ea
            k["emits_PARKED_shipped"] += ep
            k["meshDrawn_NEW"] += md
            k["actors"] = k["actors"]
            agg[(cls,c)]["_actors_set"] = 0
        # distinct actors per cat
        seen = collections.defaultdict(set)
        for cls, actor, frame, sl, alpha, status, rel, legacy in rows:
            seen[(cls, cat(int(sl),int(alpha),int(rel),int(status)))].add(actor)
        for k,v in seen.items(): agg[k]["distinct_actors"] = len(v)

for stem in sorted(out):
    print(f"\n### {stem}")
    print(f"{'class':7s} {'category':30s} {'actors':>6s} {'samp':>6s} {'emitNEW':>8s} {'emitARMED':>10s} {'emitPARKED':>11s} {'drawnNEW':>9s}")
    for (cls, c), k in sorted(out[stem].items()):
        print(f"{cls:7s} {c:30s} {k['distinct_actors']:6d} {k['samples']:6d} {k['emits_NEW']:8d} {k['emits_ARMED_LEGACY']:10d} {k['emits_PARKED_shipped']:11d} {k['meshDrawn_NEW']:9d}")
