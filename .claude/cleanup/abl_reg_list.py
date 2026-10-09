#!/usr/bin/env python3
"""abl_reg_list.py FILE... -- ordered, #if 0-aware list of live ABLi_addFunction registrations across FILEs (name + normalized argument text)."""
import re, sys
n = 0
for f in sys.argv[1:]:
    skip = 0; depth = []
    for l in open(f, "rb").read().decode("utf-8", "replace").splitlines():
        m = re.match(r"\s*#\s*(\w+)\s*(.*)", l)
        if m:
            d, rest = m.group(1), m.group(2).strip()
            if d in ("if", "ifdef", "ifndef"):
                dead = (d == "if" and re.match(r"0\s*($|//|/\*)", rest) is not None)
                depth.append(dead)
            elif d == "else" and depth: depth[-1] = not depth[-1] if depth[-1] else depth[-1]
            elif d == "endif" and depth: depth.pop()
            continue
        if any(depth): continue
        if l.lstrip().startswith("//"): continue
        m = re.search(r'ABLi_addFunction\s*\((.*)\)\s*;', l)
        if m:
            n += 1; print(n, re.sub(r"\s+", " ", m.group(1)).strip())
