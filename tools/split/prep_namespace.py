#!/usr/bin/env python3
"""What: the namespace prep a file needs before it can be split: every anonymous namespace becomes a named one, the namespace is closed and re-opened at the
requested piece boundaries, and a using-directive keeps the global-scope code that used the anonymous names compiling.
Why: names in an anonymous namespace have internal linkage, so two pieces cannot share them; a named namespace re-opened in each piece can (the water island, GuiRuntime
and terrain island preps were this edit by hand). The edit adds only namespace lines; line endings are kept.
Usage: py -3 tools/split/prep_namespace.py FILE --name NAME [--at LINE ...] [--no-using] [--dry-run]
  --at LINE   1-based line INSIDE a namespace body in front of which it is closed and re-opened (put it before the comment block of the function that starts a piece)
  The using-directive goes right after the first renamed namespace closes (skipped with --no-using).
Exit: 0 ok, 1 nothing to rename, 2 a boundary is not inside a renamed namespace body."""
import argparse, re, sys


def strip_code(line):
    """line without string / char literals and // comments, for brace scans (block comments are not tracked; fine for these sources)."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c in "\"'":
            j = i + 1
            while j < n and line[j] != c:
                j += 2 if line[j] == "\\" else 1
            i = j + 1
        elif line[i:i + 2] == "//":
            break
        else:
            out.append(c)
            i += 1
    return "".join(out)


def prep(text, name, boundaries, using=True):
    """Returns (new_text, renamed_count). Raises ValueError for a boundary outside a renamed namespace body."""
    crlf = "\r\n" in text
    lines = text.replace("\r\n", "\n").split("\n")
    depth, spans, stack = 0, [], []          # spans: (open_idx, close_idx) of anonymous namespaces
    for i, l in enumerate(lines):
        code = strip_code(l)
        if re.match(r"\s*namespace\s*\{", code):
            stack.append((depth, i))
        depth += code.count("{") - code.count("}")
        while stack and depth <= stack[-1][0]:
            _, o = stack.pop()
            spans.append((o, i))
    if not spans:
        return text, 0
    spans.sort()
    for b in boundaries:
        if not any(o < b - 1 < c for o, c in spans):
            raise ValueError("line %d is not inside an anonymous namespace body" % b)
    cuts = {b - 1 for b in boundaries}
    out = []
    first_close = spans[0][1]
    open_at = {o for o, _ in spans}
    close_at = {c for _, c in spans}
    for i, l in enumerate(lines):
        if i in cuts:
            out += ["}  // namespace " + name, "", "namespace " + name + " {", ""]
        if i in open_at:
            l = re.sub(r"namespace\s*\{", "namespace " + name + " {", l, count=1)
        elif i in close_at:
            l = re.sub(r"\}\s*(//.*)?$", "} // namespace " + name, l, count=1)
        out.append(l)
        if using and i == first_close:
            out.append("using namespace " + name + ";")
    s = "\n".join(out)
    return (s.replace("\n", "\r\n") if crlf else s), len(spans)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--name", required=True)
    ap.add_argument("--at", type=int, action="append", default=[])
    ap.add_argument("--no-using", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    text = open(a.file, "rb").read().decode("utf-8")
    try:
        new, n = prep(text, a.name, a.at, not a.no_using)
    except ValueError as e:
        print("prep_namespace:", e, file=sys.stderr)
        return 2
    if not n:
        print("prep_namespace: no anonymous namespace in %s" % a.file, file=sys.stderr)
        return 1
    print("prep_namespace: %d anonymous namespace(s) -> %s, %d boundary re-open(s)%s" % (n, a.name, len(a.at), " (dry run)" if a.dry_run else ""))
    if not a.dry_run:
        open(a.file, "wb").write(new.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
