#!/usr/bin/env python3
"""What: drops pure rule/blank comment lines (//****, //----, bare //) and collapses runs of blank lines, keeping each line's CRLF/LF bytes.
Why: the legacy files carry thousands of lines of comment scaffolding that hide real content before a split.
Usage: py -3 tools/split/strip_rules.py [--apply] FILE...   (dry run by default)"""
import re, sys
apply = "--apply" in sys.argv
RULE = re.compile(rb"^\s*//[*\-=#~_ \t]*\r?$")
for f in [a for a in sys.argv[1:] if a != "--apply"]:
    lines = open(f, "rb").read().split(b"\n")
    out = []; dropped = 0; prev_blank = False
    for l in lines:
        blank = l.strip() == b""
        if RULE.match(l): dropped += 1; continue
        if blank and prev_blank: dropped += 1; continue
        out.append(l); prev_blank = blank
    print(f"{f}: {len(lines)} -> {len(out)} lines (-{dropped})")
    if apply: open(f, "wb").write(b"\n".join(out))
