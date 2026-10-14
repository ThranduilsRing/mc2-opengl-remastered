#!/usr/bin/env python3
"""abl_writer_census - every place the engine writes into, or takes the address of, a script variable buffer, frozen with a verdict each.

WHAT: scans mclib/ablx*.cpp, mclib/ablrtn*.cpp and code/ablmc2*.cpp for (a) the calls that copy or format bytes (strcpy, strncpy, strcat,
memcpy, memset, sprintf, mc2::fmt::to_buf, abl::copyBounded / appendBounded, ...) and (b) the calls that hand a script variable's address to
C++ (ABLi_popCharPtr / ABLi_popCharBuf / ABLi_popIntegerPtr / ABLi_popRealPtr), and records each per (file, function, callee) with a count.
tools/abl_writer_census.baseline.json holds the frozen census and a reviewed verdict for every row.
WHY: a script's char[N] / int[N] / real[N] variable is a heap block of exactly N elements, so any write the engine makes into it that is not
bounded by N is a heap overwrite (mc2_15, 76204ae18). docs says "the writers are all bounded" only as long as nobody adds one: a new writer, or a
new pointer fetch, fails --check until it carries a verdict a human wrote.

  py -3 tools/abl_writer_census.py --check    # exit 1 on a new / removed / changed row or a row without a verdict (the board row)
  py -3 tools/abl_writer_census.py --write    # re-freeze after an intended change; new rows get verdict UNCLASSIFIED and must be filled in
  py -3 tools/abl_writer_census.py --report   # the rows with their line numbers, grouped by verdict, and the totals

Verdicts: bounded (the write is limited by the variable's declared size), fixed (the builtin writes a documented fixed count into a variable
the script declared with that count), open (the count depends on engine state and the declaration is trusted: the open class of the audit),
read (the address is only read), engine (the destination is a C++ buffer, not a script variable), frame (the executor's own stack-frame copy).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

BASELINE = os.path.join(HERE, "abl_writer_census.baseline.json")
SCOPE = (r"^mclib/ablx[^/]*\.cpp$", r"^mclib/ablrtn[^/]*\.cpp$", r"^code/ablmc2[^/]*\.cpp$")
VERDICTS = ("bounded", "fixed", "open", "read", "engine", "frame")

#: calls that copy or format bytes
COPIERS = ("strcpy", "strncpy", "strcat", "strncat", "memcpy", "memmove", "memset", "sprintf", "snprintf", "_snprintf", "_snprintf_s",
           "to_buf", "to_buf_n", "copyBounded", "appendBounded", "copyIntArray", "copyRealArray")
#: calls that hand a script variable's address to C++
FETCHERS = ("ABLi_popCharPtr", "ABLi_popCharBuf", "ABLi_popIntegerPtr", "ABLi_popRealPtr", "ABLi_popIntBuf", "ABLi_popRealBuf")
#: a function header at column 0 with its brace on the same line ("void execFoo (void) {"): the anchor that re-syncs the brace tracker after an
#: #ifdef that left a function's braces unbalanced (code/ablmc2_misc.cpp execIsGateOpen)
HEADER = re.compile(r"(?!(?:if|for|while|switch|else|do|catch|return|namespace|struct|class|enum|union|typedef)\b)"
                    r"[A-Za-z_][\w:<>,*&\s]*?\s[*&]*([A-Za-z_][\w:~]*)\s*\([^;{}]*\)\s*(?:const\s*)?\{")
CALL = re.compile(r"\b(%s)\s*\(" % "|".join(COPIERS + FETCHERS))


def strip_code(text: str) -> str:
    """The text with comments and the contents of string / character literals blanked, newlines kept so line numbers survive."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(q + "".join("\n" if ch == "\n" else " " for ch in text[i + 1:j]) + q)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scan(text: str):
    """Yield (line, function, callee) for every copier / fetcher call inside a function body of `text`."""
    code = strip_code(text)
    stack: list[str | None] = []  # per open brace: a function name, "" for a namespace / extern block or a type, or None for a nested block
    header_start = 0
    line = 1
    func = None
    i = 0
    while i < len(code):
        c = code[i]
        if func is not None and (i == 0 or code[i - 1] == "\n"):
            resync = HEADER.match(code, i)
            if resync:
                func = resync.group(1)
                stack = [func]
                i = resync.end()
                continue
        if c == "\n":
            line += 1
        elif c == "{":
            if func is None:
                header = code[header_start:i]
                open_paren = header.find("(")
                if re.search(r"\bnamespace\b|\bextern\s*\"C\"", header) or open_paren < 0:
                    stack.append("")
                    header_start = i + 1
                else:
                    m = re.search(r"([\w:~]+)\s*$", header[:open_paren])
                    name = m.group(1) if m else "?"
                    stack.append(name)
                    func = name
            else:
                stack.append(None)
        elif c == "}":
            if stack:
                stack.pop()
            if func is not None and not any(s for s in stack):
                func = None
            if func is None:
                header_start = i + 1
        elif c == ";" and func is None:
            header_start = i + 1
        elif func is not None:
            m = CALL.match(code, i) if (c.isalpha() or c == "_") and (i == 0 or not (code[i - 1].isalnum() or code[i - 1] == "_")) else None
            if m:
                yield line, func, m.group(1)
                i = m.end(1)
                continue
        i += 1


def tracked_sources() -> list[str]:
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], check=True, stdout=subprocess.PIPE,
                         **console_child.child_kwargs(redirected=True)).stdout.decode("utf-8")
    return sorted(p for p in out.split("\0") if p and any(re.match(rx, p) for rx in SCOPE))


def census() -> tuple[dict[str, int], dict[str, list[int]]]:
    counts: dict[str, int] = {}
    lines: dict[str, list[int]] = {}
    for rel in tracked_sources():
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
        for line, func, callee in scan(text):
            key = f"{rel}::{func}::{callee}"
            counts[key] = counts.get(key, 0) + 1
            lines.setdefault(key, []).append(line)
    return counts, lines


def load_baseline() -> dict:
    with open(BASELINE, encoding="utf-8") as fh:
        return json.load(fh)


def write_baseline(counts: dict[str, int]) -> None:
    old = load_baseline()["sites"] if os.path.isfile(BASELINE) else {}
    sites = {}
    for key in sorted(counts):
        prev = old.get(key, {})
        sites[key] = {"count": counts[key], "verdict": prev.get("verdict", "UNCLASSIFIED"), "why": prev.get("why", "")}
    doc = {
        "about": "Frozen by tools/abl_writer_census.py --write. One row per (file, function, callee) with a reviewed verdict; see the tool's docstring.",
        "scope": list(SCOPE),
        "sites": sites,
    }
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")


def summary(sites: dict) -> dict[str, int]:
    totals = {v: 0 for v in VERDICTS}
    totals["UNCLASSIFIED"] = 0
    for row in sites.values():
        totals[row["verdict"]] = totals.get(row["verdict"], 0) + row["count"]
    return totals


def check() -> int:
    counts, _ = census()
    base = load_baseline()["sites"]
    problems = []
    for key in sorted(set(counts) | set(base)):
        if key not in base:
            problems.append(f"  new row: {key} x{counts[key]}")
        elif key not in counts:
            problems.append(f"  row gone: {key} (baseline x{base[key]['count']})")
        elif counts[key] != base[key]["count"]:
            problems.append(f"  count changed: {key} baseline x{base[key]['count']}, now x{counts[key]}")
    for key, row in sorted(base.items()):
        if row["verdict"] not in VERDICTS:
            problems.append(f"  no verdict: {key} ({row['verdict']})")
        elif not row.get("why"):
            problems.append(f"  no reason for the verdict: {key}")
    if problems:
        print("abl_writer_census: the script-buffer writers differ from the frozen census:")
        print("\n".join(problems))
        print("A new writer needs a verdict (bounded / fixed / open / read / engine / frame) and a reason in tools/abl_writer_census.baseline.json: "
              "run --write, fill in the new rows, re-run --check.")
        return 1
    totals = summary(base)
    print(f"abl_writer_census: OK ({len(base)} rows, {sum(totals.values())} sites: " + ", ".join(f"{k} {v}" for k, v in totals.items() if v) + ")")
    return 0


def report() -> int:
    counts, lines = census()
    base = load_baseline()["sites"]
    by_verdict: dict[str, list[str]] = {}
    for key in sorted(counts):
        verdict = base.get(key, {}).get("verdict", "UNCLASSIFIED")
        by_verdict.setdefault(verdict, []).append(f"  {key}  x{counts[key]}  lines {','.join(map(str, lines[key]))}")
    for verdict in (*VERDICTS, "UNCLASSIFIED"):
        if verdict in by_verdict:
            print(f"[{verdict}] {len(by_verdict[verdict])} rows")
            print("\n".join(by_verdict[verdict]))
    print("totals:", ", ".join(f"{k} {v}" for k, v in summary({k: {"count": counts[k], "verdict": base.get(k, {}).get("verdict", "UNCLASSIFIED")} for k in counts}).items() if v))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--report", action="store_true")
    args = ap.parse_args()
    if args.write:
        counts, _ = census()
        write_baseline(counts)
        print(f"abl_writer_census: froze {len(counts)} rows into {os.path.relpath(BASELINE, ROOT)}")
        return 0
    return report() if args.report else check()


if __name__ == "__main__":
    sys.exit(main())
