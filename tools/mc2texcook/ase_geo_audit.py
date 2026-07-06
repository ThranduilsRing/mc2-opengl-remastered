#!/usr/bin/env python3
"""ase_geo_audit.py — vertex/triangle budget audit of MC2 .ase static/mech meshes.

Sums *MESH_NUMVERTEX / *MESH_NUMFACES across all GEOMOBJECTs per .ase, categorizes
by filename, and prints a per-category summary + heaviest meshes. Read-only.

Usage: py -3 ase_geo_audit.py [--dir A:/Games/mc2-srcdata/tgl]
"""
import argparse, re, statistics
from pathlib import Path
from collections import defaultdict

V = re.compile(r"\*MESH_NUMVERTEX\s+(\d+)")
F = re.compile(r"\*MESH_NUMFACES\s+(\d+)")

MECH = ("jaeger","urban","madcat","timber","mech","atlas","puma","cougar","thor","loki","piece")
VEH  = ("apc","truck","tank","car","transport","ambulance","carrier","lrm","jeep","hover","wheel","artiller")
TREE = ("oak","pine","palm","tree","willow","cedar","bush","foliage","fern","shrub","cactus")
TURR = ("turret","sensor","tower","antenna","gun","spotter","radar")

def categorize(stem: str) -> str:
    s = stem.lower()
    # strip common LOD/damage suffixes for keyword matching
    for kw in MECH:
        if kw in s: return "mech"
    for kw in VEH:
        if kw in s: return "vehicle"
    for kw in TREE:
        if kw in s: return "tree/foliage"
    for kw in TURR:
        if kw in s: return "turret/sensor"
    return "building/other"

def parse(p: Path):
    txt = p.read_text(errors="replace")
    verts = sum(int(m) for m in V.findall(txt))
    faces = sum(int(m) for m in F.findall(txt))
    return verts, faces

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=r"A:/Games/mc2-srcdata/tgl")
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()
    files = sorted(Path(a.dir).glob("*.ase"))
    rows = []
    for p in files:
        try:
            v, f = parse(p)
            rows.append((p.stem, categorize(p.stem), v, f))
        except Exception as e:
            print(f"  skip {p.name}: {e}")
    print(f"parsed {len(rows)} .ase from {a.dir}\n")

    cats = defaultdict(list)
    for stem, cat, v, f in rows:
        cats[cat].append((stem, v, f))

    print(f"{'category':<16}{'files':>7}{'totV':>12}{'totTris':>12}{'medV':>8}{'medT':>8}{'maxV':>8}{'maxT':>8}")
    for cat in sorted(cats, key=lambda c: -sum(r[1] for r in cats[c])):
        rs = cats[cat]
        vs = [r[1] for r in rs]; fs = [r[2] for r in rs]
        print(f"{cat:<16}{len(rs):>7}{sum(vs):>12}{sum(fs):>12}"
              f"{int(statistics.median(vs)):>8}{int(statistics.median(fs)):>8}{max(vs):>8}{max(fs):>8}")
    tv = sum(r[2] for r in rows); tf = sum(r[3] for r in rows)
    print(f"{'TOTAL':<16}{len(rows):>7}{tv:>12}{tf:>12}\n")

    print(f"Top {a.top} heaviest meshes (by triangles):")
    for stem, cat, v, f in sorted(rows, key=lambda r: -r[3])[:a.top]:
        print(f"  {f:>7} tris {v:>7} verts  [{cat}]  {stem}")

    # mech base meshes (exclude obvious animation/damage variants for a 'base' view)
    SUF = ("dam","damx","l1","l2","x","fallforward","fallbackward","fall")
    mech_base = [r for r in rows if r[1]=="mech" and not any(r[0].lower().endswith(s) for s in SUF)]
    if mech_base:
        print(f"\nMech meshes (base-ish, {len(mech_base)} of {len(cats['mech'])} mech files):")
        for stem, cat, v, f in sorted(mech_base, key=lambda r:-r[3])[:12]:
            print(f"  {f:>7} tris {v:>7} verts  {stem}")

if __name__ == "__main__":
    main()
