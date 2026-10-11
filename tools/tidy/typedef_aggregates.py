"""typedef_aggregates - rewrite `typedef struct|union|enum [Tag] { ... } Name;` to the C++ form, as a text transform.

WHAT: `typedef struct Tag {...} Name;` becomes `struct Tag {...};` plus `using Name = Tag;` (just the struct when Name == Tag), and
`typedef struct {...} Name;` becomes `struct Name {...};`. Same for union and enum. Anything else (several declarators, pointer typedefs,
function-pointer typedefs, an enum with an underlying type) is left alone.
WHY: modernize-use-using would turn these into `using Name = struct {...};`, which MSVC rejects, so clang-tidy cannot do them. The rewrite
is a text edit like the others in the tidy program: it is kept only for files whose objects (and every including object, for a header)
come out byte-identical under tools/objhash.py, and reverted otherwise.
"""
from __future__ import annotations

import re

_HEAD = re.compile(r"^(?P<indent>[ \t]*)typedef[ \t]+(?P<kw>struct|union|enum)(?:[ \t]+(?P<tag>[A-Za-z_]\w*))?[ \t\r\n]*\{", re.M)
_TAIL = re.compile(r"[ \t\r\n]*(?P<name>[A-Za-z_]\w*)[ \t]*;")


def _dead_ranges(text: str):
    """[(start, end)] of comments, string literals and character literals, so braces and keywords inside them are ignored."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append((i, j))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append((i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append((i, min(j + 1, n)))
            i = j + 1
        else:
            i += 1
    return out


def _matching_brace(text: str, open_at: int, dead) -> int:
    """Index of the `}` that closes the `{` at open_at, or -1."""
    depth, i, n, k = 0, open_at, len(text), 0
    while i < n:
        while k < len(dead) and dead[k][1] <= i:
            k += 1
        if k < len(dead) and dead[k][0] <= i < dead[k][1]:
            i = dead[k][1]
            continue
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _in(dead, pos: int) -> bool:
    return any(a <= pos < b for a, b in dead)


def rewrite(text: str) -> str:
    """The text with every eligible aggregate typedef rewritten (unchanged when none is eligible)."""
    dead = _dead_ranges(text)
    edits = []        # (start, end, replacement), non-overlapping, applied from the end
    for m in _HEAD.finditer(text):
        if _in(dead, m.start("indent") + len(m.group("indent"))):
            continue
        open_at = m.end() - 1
        close = _matching_brace(text, open_at, dead)
        if close < 0:
            continue
        t = _TAIL.match(text, close + 1)
        if not t:
            continue
        kw, tag, name, indent = m.group("kw"), m.group("tag"), t.group("name"), m.group("indent")
        head_end = m.end()
        body_head = f"{indent}{kw} {tag or name} {{"
        if tag and tag != name:
            tail = f";\n{indent}using {name} = {tag};"
        else:
            tail = ";"
        # edits are applied from the end so earlier offsets stay valid; the body between them is never touched
        edits.append((close + 1, t.end(), tail))
        edits.append((m.start(), head_end, body_head))
    out = text
    for start, end, rep in sorted(edits, reverse=True):
        out = out[:start] + rep + out[end:]
    return out


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as fh:
            src = fh.read()
        new = rewrite(src)
        print(f"{path}: {'rewritten' if new != src else 'unchanged'}")
        if new != src:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as fh:
                fh.write(new)
