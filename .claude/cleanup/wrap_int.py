#!/usr/bin/env python3
"""wrap_int.py LISTFILE -- LISTFILE lines 'path line'; wrap the narrowing RHS (X.GetLength() / strlen(..)) in static_cast<int>. Byte-preserving."""
import re, sys, collections
by = collections.defaultdict(set)
for ln in open(sys.argv[1]):
    p, n = ln.split(); by[p].add(int(n))
pat = re.compile(rb'(?<![\w>])((?:\w+(?:\[\w+\])?)(?:\.|->)GetLength\(\)|strlen\(\w+\))')
bad = 0
for p, ns in by.items():
    data = open(p, "rb").read().split(b"\n")
    for n in ns:
        l = data[n-1]
        t = pat.sub(rb'static_cast<int>(\1)', l, count=1)
        if t == l:
            print("NOMATCH", p, n, l); bad += 1
        data[n-1] = t
    open(p, "wb").write(b"\n".join(data))
print("bad", bad)
