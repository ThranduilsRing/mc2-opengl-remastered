#!/usr/bin/env python3
"""subst.py FILE LINE OLD NEW -- replace OLD with NEW on 1-based LINE (byte-preserving, CRLF-safe); fails if OLD absent."""
import sys
f, ln, old, new = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
data = open(f, "rb").read().split(b"\n")
l = data[ln-1]
o = old.encode()
if o not in l: sys.exit(f"{f}:{ln}: pattern not found: {l!r}")
data[ln-1] = l.replace(o, new.encode(), 1)
open(f, "wb").write(b"\n".join(data))
