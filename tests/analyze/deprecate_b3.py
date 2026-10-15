#!/usr/bin/env python3
"""deprecate_b3 - mark the step 4 B3 file-and-stream ownership API [[deprecated("B3")]] for a census build, and take the mark off again.

WHAT: `on` puts `[[deprecated("B3")]]` in front of the declarations through which a File-family object or its buffers change hands: the class-specific
`operator new` / `operator delete` of File and ABLFile (so every `new File`, `new PacketFile`, `new FitIniFile`, `new ABLFile` and every `delete` of one, through
a derived class too, is reported), `File::getFilename` (a pointer into the object's own name buffer), the parent / child API (`getParent`, `addChild`,
`removeChild`, the three `open(parent, size)` overrides) and `File::readRAW` (a heap-routed raw read). `off` removes exactly those insertions and nothing else.
WHY: the compiler, not grep, finds every owner, including the ones that go through a typedef (FilePtr, FitIniFilePtr, PacketFilePtr), a derived class or a
member that is deleted far from its `new`. file_callsites.py turns the build's C4996 warnings into the committed list. The mark is a LOCAL EDIT and is never
committed (`git diff --stat` must be clean of these four headers before any commit).

  py -3 tests/analyze/deprecate_b3.py on
  py -3 tests/analyze/deprecate_b3.py off
Line endings are kept byte for byte (the headers are CRLF).
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
MARK = '[[deprecated("B3")]] '

#: header -> the declarations to mark, as regexes on the text after the leading whitespace. Each must match exactly `count` lines, so a header edit that
#: moves or renames a declaration makes `on` fail instead of silently shrinking the census.
TARGETS = {
    "mclib/file.h": [
        (r"void \*operator new \(", 1),
        (r"void operator delete \(", 1),
        (r"virtual long open \(File \*_parent", 1),
        (r"long readRAW \(DWORD\*", 1),
        (r"char\* getFilename \(", 1),
        (r"FilePtr getParent \(", 1),
        (r"long addChild \(", 1),
        (r"void removeChild \(", 1),
    ],
    "mclib/packet.h": [(r"virtual long open \(FilePtr _parent", 1)],
    "mclib/inifile.h": [(r"virtual long open \(FilePtr _parent", 1)],
    "mclib/ablscan.h": [(r"void\* operator new \(", 1), (r"void operator delete \(", 1)],
}


def on() -> int:
    total = 0
    for path, wanted in TARGETS.items():
        full = os.path.join(ROOT, path)
        with open(full, "rb") as f:
            lines = f.read().split(b"\n")
        for pattern, count in wanted:
            hits = [i for i, raw in enumerate(lines) if re.match(r"^\s*(" + pattern + r")", raw.decode("latin-1"))]
            if len(hits) != count:
                print(f"deprecate_b3: {path}: '{pattern}' matched {len(hits)} line(s), expected {count}", file=sys.stderr)
                return 1
            for i in hits:
                text = lines[i].decode("latin-1")
                if MARK in text:
                    continue
                m = re.match(r"^(\s*)", text)
                lines[i] = (m[1] + MARK + text[len(m[1]):]).encode("latin-1")
                total += 1
        with open(full, "wb") as f:
            f.write(b"\n".join(lines))
    print(f"deprecate_b3: {total} marks inserted")
    return 0


def off() -> int:
    removed = 0
    mark = MARK.encode("latin-1")
    for path in TARGETS:
        full = os.path.join(ROOT, path)
        with open(full, "rb") as f:
            data = f.read()
        removed += data.count(mark)
        with open(full, "wb") as f:
            f.write(data.replace(mark, b""))
    print(f"deprecate_b3: {removed} marks removed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("on", "off"):
        sys.exit(__doc__)
    sys.exit(on() if sys.argv[1] == "on" else off())
