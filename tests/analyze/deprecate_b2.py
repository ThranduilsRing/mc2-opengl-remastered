#!/usr/bin/env python3
"""deprecate_b2 - mark the step 4 B2 container types [[deprecated("B2")]] for a census build, and take the mark off again.

WHAT: `on` inserts `[[deprecated("B2")]]` before the class and before every public member of EList, SortList, PriorityQueue, BitFlag/ByteFlag and before
MoverGroup::handleTacticalOrder / sortMovers; `off` removes exactly those insertions (and nothing else, so unrelated edits to the same files survive).
WHY: the compiler, not grep, finds every use, including the ones that go through an alias (COMPONENT_LIST), a derived class (CObjective), an `auto`
iterator or an inherited unqualified call. container_callsites.py turns the build's C4996 warnings into the committed list. The mark is a LOCAL EDIT
and is never committed (`git diff --stat` must be clean of these four headers before any commit).

  py -3 tests/analyze/deprecate_b2.py on
  py -3 tests/analyze/deprecate_b2.py off
Line endings are kept byte for byte (the headers are CRLF).
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
MARK = '[[deprecated("B2")]] '
FILES = ["mclib/elist.h", "mclib/sortlist.h", "mclib/pqueue.h", "mclib/bitflag.h", "code/group.h"]


def _lines(path: str) -> list[bytes]:
    with open(os.path.join(ROOT, path), "rb") as f:
        return f.read().split(b"\n")


def _write(path: str, lines: list[bytes]) -> None:
    with open(os.path.join(ROOT, path), "wb") as f:
        f.write(b"\n".join(lines))


def _mark(lines: list[bytes], idx: int, pattern: str, count: list[int]) -> None:
    """Put the mark in front of the first match of `pattern` (a regex on the text after the leading whitespace) on line idx."""
    text = lines[idx].decode("latin-1")
    m = re.match(r"^(\s*)(" + pattern + r")", text)
    if m and MARK not in text:
        lines[idx] = (m[1] + MARK + text[len(m[1]):]).encode("latin-1")
        count[0] += 1


def on() -> int:
    count = [0]
    # EList: the class and every public member declared between "inline EList();" and the protected section.
    lines = _lines("mclib/elist.h")
    in_public = False
    for i, raw in enumerate(lines):
        t = raw.decode("latin-1")
        if t.startswith("ELIST_TPL_DEF class EList"):
            lines[i] = t.replace("class EList", "class " + MARK + "EList", 1).encode("latin-1")
            count[0] += 1
        if re.match(r"^\s*inline\s+EList\(\);", t):
            in_public = True
        if in_public and re.match(r"^protected:", t):
            in_public = False
        if in_public:
            _mark(lines, i, r"inline\s", count)
    _write("mclib/elist.h", lines)
    # SortList and PriorityQueue: the class and its public members (two-tab declarations).
    for path, cls, public_from, member in (
        ("mclib/sortlist.h", "SortList", "public:", r"(void\b|long\b|float\b|SortList\b|~SortList\b)"),
        ("mclib/pqueue.h", "PriorityQueue", "public:", r"(void\b|int\b|bool\b|PQNode\*|PriorityQueue\b|~PriorityQueue\b)"),
    ):
        lines = _lines(path)
        in_public = False
        for i, raw in enumerate(lines):
            t = raw.decode("latin-1")
            if re.match(r"^class " + cls + r"\b", t) and MARK not in t:
                lines[i] = t.replace("class " + cls, "class " + MARK + cls, 1).encode("latin-1")
                count[0] += 1
            if t.strip() == public_from:
                in_public = True
            if in_public and re.match(r"^};", t):
                in_public = False
            if in_public and t.startswith("\t\t") and not t.startswith("\t\t\t"):
                _mark(lines, i, member, count)
        _write(path, lines)
    lines = _lines("mclib/bitflag.h")
    for i, raw in enumerate(lines):
        t = raw.decode("latin-1")
        m = re.match(r"^class (BitFlag|ByteFlag)\b", t)
        if m and MARK not in t:
            lines[i] = t.replace("class " + m[1], "class " + MARK + m[1], 1).encode("latin-1")
            count[0] += 1
    _write("mclib/bitflag.h", lines)
    lines = _lines("code/group.h")
    for i, raw in enumerate(lines):
        t = raw.decode("latin-1")
        if "long handleTacticalOrder (" in t:
            _mark(lines, i, r"virtual\s+long", count)
        if "static void sortMovers (" in t:
            _mark(lines, i, r"static\s+void", count)
    _write("code/group.h", lines)
    print(f"deprecate_b2: {count[0]} marks inserted")
    return 0 if count[0] else 1


def off() -> int:
    removed = 0
    mark = MARK.encode("latin-1")
    for path in FILES:
        with open(os.path.join(ROOT, path), "rb") as f:
            data = f.read()
        removed += data.count(mark)
        data = data.replace(mark, b"")
        with open(os.path.join(ROOT, path), "wb") as f:
            f.write(data)
    print(f"deprecate_b2: {removed} marks removed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("on", "off"):
        sys.exit(__doc__)
    sys.exit(on() if sys.argv[1] == "on" else off())
