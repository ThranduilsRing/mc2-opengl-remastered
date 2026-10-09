#!/usr/bin/env python3
"""Tabulate MSVC warnings from a build log by code x origin file (header where the line lives)."""
import re, sys, collections
log = sys.argv[1]
codes = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else None
rx = re.compile(r'^\s*([A-Za-z]:.+?)\((\d+)(?:,\d+)?\): warning (C\d+)')
tot = collections.Counter(); by = collections.Counter(); lines = collections.defaultdict(collections.Counter)
for ln in open(log, errors="replace"):
    m = rx.match(ln)
    if not m: continue
    f, n, c = m.groups()
    f = f.replace("\\", "/").replace("A:/Games/mc2-lane2/", "")
    if codes and c not in codes: continue
    tot[c] += 1; by[(c, f)] += 1; lines[(c, f)][int(n)] += 1
print("TOTALS:", dict(tot.most_common()))
for (c, f), n in sorted(by.items(), key=lambda kv: (-kv[1])):
    print(f"| {c} | {f} | {n} | {len(lines[(c,f)])} |")
