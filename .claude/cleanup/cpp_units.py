#!/usr/bin/env python3
"""cpp_units.py FILE -- split a C++ file into top-level units (leading comments/blank lines + one declaration/definition,
or a whole top-level preprocessor #if...#endif region). Prints: start end kind name. Library: parse(path)->list of dicts."""
import re, sys

def strip_line(l, st):
    """remove strings/chars/comments from a line; st = {'blk': bool}. returns code text."""
    out = []; i = 0; n = len(l)
    while i < n:
        if st["blk"]:
            j = l.find("*/", i)
            if j < 0: return "".join(out)
            st["blk"] = False; i = j + 2; continue
        c = l[i]
        if c == "/" and i + 1 < n and l[i+1] == "/": break
        if c == "/" and i + 1 < n and l[i+1] == "*": st["blk"] = True; i += 2; continue
        if c == '"':
            i += 1
            while i < n and l[i] != '"':
                i += 2 if l[i] == "\\" else 1
            i += 1; out.append('""'); continue
        if c == "'":
            i += 1
            while i < n and l[i] != "'":
                i += 2 if l[i] == "\\" else 1
            i += 1; out.append("''"); continue
        out.append(c); i += 1
    return "".join(out)

def parse(path):
    raw = open(path, "rb").read().decode("utf-8", "replace")
    lines = raw.split("\n")
    st = {"blk": False}; depth = 0; pp = 0; units = []; cur_start = 0
    cont = False  # preprocessor line continuation
    code_seen = False; name = None
    for i, l in enumerate(lines):
        t = l.rstrip("\r")
        if cont:
            cont = t.endswith("\\"); 
            continue
        s = t.strip()
        if s.startswith("#") and not st["blk"]:
            m = re.match(r"#\s*(\w+)", s); d = m.group(1) if m else ""
            if d in ("if", "ifdef", "ifndef"): pp += 1
            elif d == "endif": pp -= 1
            cont = t.endswith("\\")
            code_seen = code_seen or depth > 0
            if depth == 0 and pp == 0 and d in ("endif", "include", "define", "undef", "pragma", "error") and not code_seen:
                units.append({"start": cur_start, "end": i, "kind": "pp", "name": s[:60]}); cur_start = i + 1
            continue
        code = strip_line(t, st)
        if code.strip(): code_seen = True
        for ch in code:
            if ch == "{": depth += 1
            elif ch == "}": depth -= 1
        end_here = False
        if t == "}" and depth != 0 and pp == 0:
            depth = 0   # brace-unbalanced inactive #if branch: a column-0 '}' closes the definition
            end_here = True
        elif depth == 0 and code_seen and pp == 0:
            if code.rstrip().endswith("}") or code.rstrip().endswith(";") or code.rstrip().endswith("};"):
                end_here = True
        if end_here:
            body = "\n".join(x.rstrip("\r") for x in lines[cur_start:i+1])
            m = re.search(r"\b(\w+)\s*\([^;{]*\)\s*(const)?\s*\{", body) or re.search(r"\b(\w+)\s*(\[[^\]]*\])?\s*(=|;)", body)
            nm = m.group(1) if m else "?"
            kind = "func" if re.search(r"\)\s*(const)?\s*\{", body) else "decl"
            units.append({"start": cur_start, "end": i, "kind": kind, "name": nm})
            cur_start = i + 1; code_seen = False
    if cur_start < len(lines):
        units.append({"start": cur_start, "end": len(lines) - 1, "kind": "tail", "name": "tail"})
    return lines, units

if __name__ == "__main__":
    lines, units = parse(sys.argv[1])
    for u in units:
        print(u["start"] + 1, u["end"] + 1, u["end"] - u["start"] + 1, u["kind"], u["name"])
