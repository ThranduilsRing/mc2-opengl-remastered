#!/usr/bin/env python3
"""axis_migrate_cpp.py -- rewrite hand-typed C++ axis swaps `o.x = -i.x; o.y = i.z; o.z = i.y;` to `mc2ToRender(o, i);`.

What: finds the same three-adjacent-assignment sites tools/axis_swizzle_ratchet.py counts as `assign-triple`
(member access .x/->x only) and replaces each by one call to RenderCore/axis_convert.h, adding the include.
A site is left alone (and listed) when comments, braces or preprocessor lines sit between its statements, when the
components use [index] access, or when the file's detected line style is mixed in the span.
Why: D11 -- one typed spelling of (x, y, z) -> (-x, z, y); the ratchet counts what is left.
Usage: py -3 tools/axis_migrate_cpp.py [--apply] FILE...     (dry run by default)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import axis_swizzle_ratchet as ax  # noqa: E402

INCLUDE_TMPL = '#include "%sRenderCore/axis_convert.h"'


def include_prefix(path):
    p = path.replace("\\", "/")
    depth = p.count("/")
    return "../" * depth


def migrate(path, apply):
    raw = open(path, "rb").read()
    text = raw.decode("utf-8", "surrogateescape")
    clean = ax.strip_comments_strings(text)
    asg = list(ax._ASSIGN.finditer(clean))
    edits, skipped = [], []
    i = 0
    while i < len(asg) - 2:
        trio = asg[i:i + 3]
        ok = ax._adjacent(clean, trio)
        if ok:
            lb = {ax._norm(t.group("lb")) for t in trio}
            rb = {ax._norm(t.group("rb")) for t in trio}
            got = {(ax._cidx(t, "l"), ax._cidx(t, "r"), bool(t.group("neg"))) for t in trio}
            if len(lb) == 1 and len(rb) == 1 and lb != rb and got == {(0, 0, True), (1, 2, False), (2, 1, False)}:
                a, b = trio[0].start(), trio[2].end()
                line = text.count("\n", 0, a) + 1
                span_raw, span_clean = text[a:b], clean[a:b]
                member = all(not t.group("li") and not t.group("ri") for t in trio)
                comments = re.findall(r"//[^\r\n]*", span_raw)
                only_line_comments = ("/*" not in span_raw
                                      and re.sub(r"//[^\r\n]*", lambda m: " " * len(m.group(0)), span_raw) == span_clean)
                if (span_raw != span_clean and not only_line_comments) or "{" in span_clean or "}" in span_clean or "#" in span_clean:
                    skipped.append((line, "block comment, brace or preprocessor between the statements"))
                elif not member:
                    skipped.append((line, "[index] component access"))
                else:
                    lbs = re.sub(r"\s+", "", trio[0].group("lb")) if False else re.sub(r"\s+", " ", trio[0].group("lb")).strip()
                    rbs = re.sub(r"\s+", " ", trio[0].group("rb")).strip()
                    # compact member spelling (a . b -> a.b)
                    lbs = re.sub(r"\s*(\.|->)\s*", r"\1", lbs)
                    rbs = re.sub(r"\s*(\.|->)\s*", r"\1", rbs)
                    rep = "mc2ToRender(%s, %s);" % (lbs, rbs)
                    if comments:
                        rep += "\t" + " ".join(c.rstrip() for c in comments)   # keep the line comments the statements carried
                    edits.append((a, b, rep, line))
                i += 3
                continue
        i += 1
    print("%s: %d site(s), %d skipped%s" % (path, len(edits), len(skipped), "" if apply else " (dry run)"))
    for line, why in skipped:
        print("   skip line %d: %s" % (line, why))
    if not edits:
        return 0
    out = text
    for a, b, rep, line in reversed(edits):
        out = out[:a] + rep + out[b:]
    if "axis_convert.h" not in out:
        lines = out.split("\n")
        last = -1
        for k, l in enumerate(lines[:200]):
            if re.match(r"\s*#\s*include\b", l) and not l.lstrip().startswith("#include <"):
                last = k
        if last < 0:
            for k, l in enumerate(lines[:200]):
                if re.match(r"\s*#\s*include\b", l):
                    last = k
        eol = "\r" if (lines[last].endswith("\r") if last >= 0 else "\r\n" in text) else ""
        inc = INCLUDE_TMPL % include_prefix(path) + eol
        lines.insert(last + 1, inc)
        out = "\n".join(lines)
    if apply:
        open(path, "wb").write(out.encode("utf-8", "surrogateescape"))
    return len(edits)


if __name__ == "__main__":
    files = [a for a in sys.argv[1:] if a != "--apply"]
    total = sum(migrate(f, "--apply" in sys.argv) for f in files)
    print("total", total)
