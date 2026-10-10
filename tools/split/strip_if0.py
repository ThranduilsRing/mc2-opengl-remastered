#!/usr/bin/env python3
"""What: removes '#if 0' blocks longer than MIN lines (nesting aware); '#if 0 ... #else' keeps the #else branch; blocks with #elif are reported only.
Why: dead '#if 0' code is deleted before a split so it is not carried into the new files; byte-preserving.
Usage: py -3 tools/split/strip_if0.py [--apply] MIN FILE...   (dry run by default)"""
import sys, re
apply = "--apply" in sys.argv
args = [a for a in sys.argv[1:] if a != "--apply"]
minlen = int(args[0]); files = args[1:]
tot = 0
for f in files:
    raw = open(f, "rb").read()
    nl = b"\r\n" if raw.count(b"\r\n") > raw.count(b"\n") / 2 else b"\n"
    lines = raw.split(b"\n")
    out = []; i = 0; removed = 0; blocks = []
    def dir_of(l):
        m = re.match(rb"\s*#\s*(\w+)", l)
        return m.group(1) if m else None
    while i < len(lines):
        l = lines[i]
        if re.match(rb"\s*#\s*if\s+0\s*(//.*|/\*.*)?\r?$", l):
            depth = 1; j = i + 1; else_at = None; elif_seen = False
            while j < len(lines) and depth:
                d = dir_of(lines[j])
                if d in (b"if", b"ifdef", b"ifndef"): depth += 1
                elif d == b"endif": depth -= 1
                elif d == b"else" and depth == 1: else_at = j
                elif d == b"elif" and depth == 1: elif_seen = True
                j += 1
            end = j - 1  # index of matching #endif
            span = end - i + 1
            if span > minlen and not elif_seen and depth == 0:
                if else_at is None:
                    removed += span; blocks.append((i + 1, span, "drop"))
                    i = end + 1; continue
                else:
                    removed += (else_at - i + 1) + 1
                    blocks.append((i + 1, span, "keep-else"))
                    out.extend(lines[else_at + 1:end]); i = end + 1; continue
            elif elif_seen and span > minlen:
                blocks.append((i + 1, span, "ELIF-SKIPPED"))
        out.append(l); i += 1
    if blocks:
        print(f"{f}: " + ", ".join(f"L{a}+{b}({c})" for a, b, c in blocks) + f"  -> -{removed} lines")
        tot += removed
        if apply: open(f, "wb").write(b"\n".join(out))
print("total lines removed:", tot, "(applied)" if apply else "(dry run)")
