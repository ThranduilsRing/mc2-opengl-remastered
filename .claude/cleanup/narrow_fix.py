#!/usr/bin/env python3
"""narrow_fix.py SITES -- apply static_cast<T>(RHS) at same-line assignment/init/return narrowing sites.

SITES lines: 'path:line CODE 'kind': conversion from 'S' to 'T' | src'. Prints NOMATCH for sites needing manual edits.
Byte-preserving (CRLF safe); sites sharing a line are applied once."""
import re, sys, collections
sites = collections.OrderedDict()
for ln in open(sys.argv[1], encoding="utf-8"):
    m = re.match(r"(\S+):(\d+) (C\d+) '([^']+)': conversion from '[^']+' to '([^']+)'", ln)
    if not m: continue
    f, l, code, kind, to = m.groups()
    to = to.replace("const ", "")
    sites.setdefault((f, int(l)), (kind, to))
files = collections.defaultdict(list)
for (f, l), v in sites.items(): files[f].append((l, v))
SKIP = set(a for a in sys.argv[2:])
bad = []
for f, items in files.items():
    data = open(f, "rb").read().split(b"\n")
    for l, (kind, to) in items:
        if f"{f}:{l}" in SKIP: continue
        raw = data[l-1]
        eol = b"\r" if raw.endswith(b"\r") else b""
        s = raw[:len(raw)-len(eol)].decode("latin1")
        T = to
        new = None
        if kind == "return":
            m = re.match(r"^(\s*return)\s*(.*?)\s*;(\s*(//.*)?)$", s)
            if m and "?" not in m.group(2):
                new = f"{m.group(1)} static_cast<{T}>({m.group(2)});{m.group(3)}"
        elif kind in ("initializing", "=", "+=", "/="):
            op = {"+=": r"\+=", "/=": r"/=", "=": r"(?<![=!<>+\-*/%&|^])=(?!=)", "initializing": r"(?<![=!<>+\-*/%&|^])=(?!=)"}[kind]
            ms = list(re.finditer(op, s))
            if ms:
                m = ms[0] if kind != "initializing" else ms[-1]
                tail = s[m.end():]
                semi = tail.rfind(";")
                if semi >= 0 and not tail[:semi].count("(") != tail[:semi].count(")"):
                    rhs = tail[:semi].strip()
                    if rhs and "static_cast" not in rhs:
                        new = s[:m.end()] + f" static_cast<{T}>({rhs})" + tail[semi:]
        if new is None:
            bad.append((f, l, kind, to, s.strip())); continue
        data[l-1] = new.encode("latin1") + eol
    open(f, "wb").write(b"\n".join(data))
for b in bad: print("MANUAL", *b)
print("manual:", len(bad), "of", len(sites))
