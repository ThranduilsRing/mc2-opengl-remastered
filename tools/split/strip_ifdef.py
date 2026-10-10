#!/usr/bin/env python3
"""What: removes conditional blocks guarded by macros that nothing defines ('#ifdef M', '#if M', '#if defined(M)'); the '#else' branch is kept; blocks with '#elif' are reported only.
With the macro name '1', '#if 1' blocks are unwrapped instead: the directives and the '#else' branch go, the first branch stays.
Why: code behind a never-defined macro is dead, and carrying it into a split only spreads it; byte-preserving (each line keeps its CRLF/LF).
Usage: py -3 tools/split/strip_ifdef.py [--apply] MACRO[,MACRO...] FILE...   (dry run by default; prove each macro is undefined everywhere first)"""
import re, sys

apply = "--apply" in sys.argv
args = [a for a in sys.argv[1:] if a != "--apply"]
macros = set(args[0].split(","))
files = args[1:]
OPEN = re.compile(rb"^\s*#\s*(if|ifdef|ifndef)\b(.*)$")
ELSE = re.compile(rb"^\s*#\s*else\b")
ELIF = re.compile(rb"^\s*#\s*elif\b")
END = re.compile(rb"^\s*#\s*endif\b")


def guarded(directive, rest):
    rest = rest.split(b"//")[0].strip()
    if directive == b"ifdef":
        return rest.decode() in macros
    if directive == b"if" and "1" in macros and rest == b"1":
        return True
    if directive == b"if":
        m = re.fullmatch(rb"(?:defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)|(\w+))", rest)
        return bool(m) and (m.group(1) or m.group(2) or m.group(3)).decode() in macros
    return False


tot = 0
for f in files:
    raw = open(f, "rb").read().split(b"\n")
    out, i, removed, report = [], 0, 0, []
    while i < len(raw):
        m = OPEN.match(raw[i])
        if not (m and guarded(m.group(1), m.group(2))):
            out.append(raw[i]); i += 1; continue
        depth, j, else_at, elif_at = 1, i + 1, None, None
        while j < len(raw) and depth:
            if OPEN.match(raw[j]):
                depth += 1
            elif END.match(raw[j]):
                depth -= 1
                if depth == 0:
                    break
            elif depth == 1 and ELSE.match(raw[j]):
                else_at = j
            elif depth == 1 and ELIF.match(raw[j]):
                elif_at = j
            j += 1
        if elif_at is not None:
            report.append(i + 1); out.append(raw[i]); i += 1; continue
        if m.group(2).split(b"//")[0].strip() == b"1" and m.group(1) == b"if":
            out.extend(raw[i + 1:else_at if else_at is not None else j])
            removed += 2 if else_at is None else j - i + 1 - (else_at - i - 1)
            i = j + 1
            continue
        if else_at is not None:
            out.extend(raw[else_at + 1:j])
        removed += j - i + 1 - (j - else_at - 1 if else_at is not None else 0)
        i = j + 1
    print("%s: %d lines removed%s" % (f, removed, ("; #elif blocks left at " + str(report)) if report else ""))
    tot += removed
    if apply:
        open(f, "wb").write(b"\n".join(out))
print("total lines removed: %d%s" % (tot, "" if apply else " (dry run)"))
