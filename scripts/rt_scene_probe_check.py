#!/usr/bin/env python3
"""Parse the VK_RT_SCENE material probe line(s) from an MC2 log (MC2_VK_RT_SELFTEST=1).

Exit 0 when at least one `material result=` line exists and every one is PASS with props, vegetation and terrain all found, holes > 0 and
false_holes == opaque_diffs == 0. Exit 1 otherwise. A `material miss` line whose instance (custom=) or material row (row=) differs from the
expected value also fails, whatever the result line says; `material info` lines (same instance and row, different triangle) and prim_info are
information only. See docs/rt-scene-contract.md.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"material result=(PASS|FAIL)(.*)")
MISS = re.compile(r"material miss .*?custom=(0x[0-9a-f]+)/(0x[0-9a-f]+) row=(\d+)/(\d+)")
FIELD = re.compile(r"(\w+)=(\d+)/(\d+)|(\w+)=(\S+)")


def parse(text: str) -> list[dict]:
    rows = []
    for m in LINE.finditer(text):
        row: dict = {"result": m.group(1)}
        for f in FIELD.finditer(m.group(2)):
            if f.group(1):
                row[f.group(1)] = (int(f.group(2)), int(f.group(3)))
            else:
                row[f.group(4)] = f.group(5)
        rows.append(row)
    return rows


def verdict(row: dict) -> list[str]:
    problems = []
    if row.get("result") != "PASS":
        problems.append(f"result={row.get('result')} reason={row.get('reason', '-')}")
    for k in ("props", "vegetation", "terrain"):
        got, want = row.get(k, (0, 0))
        if want == 0 or got != want:
            problems.append(f"{k}={got}/{want}")
    if int(row.get("holes", 0)) <= 0:
        problems.append("holes=0")
    for k in ("false_holes", "opaque_diffs"):
        if int(row.get(k, 1)) != 0:
            problems.append(f"{k}={row.get(k)}")
    return problems


def check(text: str) -> tuple[bool, list[str]]:
    rows = parse(text)
    if not rows:
        return False, ["no 'material result=' line"]
    msgs = []
    for m in MISS.finditer(text):
        if m.group(1) != m.group(2) or m.group(3) != m.group(4):
            msgs.append(f"wrong instance or material row: custom={m.group(1)}/{m.group(2)} row={m.group(3)}/{m.group(4)}")
    for i, r in enumerate(rows):
        msgs += [f"line {i}: {p}" for p in verdict(r)]
    return not msgs, msgs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logs", nargs="+", type=Path)
    a = ap.parse_args(argv)
    ok = True
    for p in a.logs:
        good, msgs = check(p.read_text(encoding="utf-8", errors="replace"))
        print(f"{p}: {'PASS' if good else 'FAIL'}" + "".join(f"\n  {m}" for m in msgs))
        ok &= good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
