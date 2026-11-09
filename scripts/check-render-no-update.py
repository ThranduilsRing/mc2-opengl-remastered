#!/usr/bin/env python3
"""RENDER-THREAD-R2 item 3: the render path never calls appearance->update().

The render thread split needs draw (renderView / Appearance::render) to be read-only on
appearance state. update() runs at sim or presentation time (Mission::updatePresentationOnly)
and the extract builds the RenderList from its result. This row fails if a call to
update / updateGeometry / updatePresentation / updateAppearancesOnly appears (comments and
strings stripped) inside any ::renderView body or in the Appearance render translation units.
Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RENDER_TUS = ["mclib/mech3d_render.cpp", "mclib/bdactor_render.cpp", "mclib/gvactor_render.cpp"]
BODY_GLOBS = ["code/*.cpp"]
FORBIDDEN = re.compile(r"\b(update|updateGeometry|updatePresentation|updateAppearancesOnly)\s*\(")
RENDERVIEW = re.compile(r"\b\w+::renderView\s*\(")


def strip(text: str) -> str:
    def blank(m: re.Match) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"', blank, text, flags=re.S)


def renderview_bodies(code: str):
    """Yield (start_line, body_text) for each ::renderView definition."""
    for m in RENDERVIEW.finditer(code):
        brace = code.find("{", m.end())
        semi = code.find(";", m.end())
        if brace < 0 or (0 <= semi < brace):
            continue  # declaration only
        depth, i = 0, brace
        while i < len(code):
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield code.count("\n", 0, m.start()) + 1, code[m.start():i + 1], code.count("\n", 0, m.start())


def violations_in(code: str, whole_file: bool, label: str) -> list[str]:
    out = []
    spans = [(0, code, 0)] if whole_file else [(s, b, off) for s, b, off in renderview_bodies(code)]
    for _start, body, off in spans:
        for mm in FORBIDDEN.finditer(body):
            line = off + body.count("\n", 0, mm.start()) + 1
            out.append(f"{label}:{line}: render path calls {mm.group(1)}()")
    return out


def scan(root: Path) -> list[str]:
    fails: list[str] = []
    for rel in RENDER_TUS:
        p = root / rel
        if p.exists():
            fails += violations_in(strip(p.read_text(encoding="utf-8", errors="replace")), True, rel)
    for g in BODY_GLOBS:
        for p in sorted(root.glob(g)):
            fails += violations_in(strip(p.read_text(encoding="utf-8", errors="replace")), False,
                                   p.relative_to(root).as_posix())
    return fails


def main() -> int:
    fails = scan(ROOT)
    for f in fails:
        print("FAIL " + f)
    if not fails and "--quiet" not in sys.argv:
        print("render-no-update: PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
