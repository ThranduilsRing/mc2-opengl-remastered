#!/usr/bin/env python3
"""strip_rules.py [--apply] FILE... -- drop pure rule/blank comment lines (//****, //----, bare //) and collapse runs of blank lines
to one. Per-line EOL bytes are preserved (mixed CRLF/LF files stay as they are)."""
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
