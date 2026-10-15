#!/usr/bin/env python3
"""journal_comment_ratchet - keep work-journal text out of C++ comments.

Comments say WHAT the code does and WHY (owner rule). A journal comment records instead WHEN or BY WHOM a line
was written: a batch, slice, step or phase tag ("B2.4", "step 4", "Slice 3"), a planning or review document
(".claude/...", "notes section 8", "BUGS row 21"), a review item id ("RB2", "(H18)", "review F3") or an agent /
lane name ("Sonnet", "Codex", "lane 1"). That text is stale once the batch closes and the reader cannot act on it;
the commit message and the lane state files are where it belongs.

Two checks:
  --check            whole-tree count of journal comments in C++ sources (the COMMITTED tree, --rev, default HEAD)
                     may not rise above the baseline; legacy hits are frozen there and only go down by intent.
  --diff BASE        every journal comment on a line ADDED since BASE (git diff BASE..HEAD, or --staged) fails,
                     whatever the baseline says: new code never adds one.

  py -3 tools/journal_comment_ratchet.py                       # report: hits per pattern and per file
  py -3 tools/journal_comment_ratchet.py --list                # every hit, file:line: pattern: text
  py -3 tools/journal_comment_ratchet.py --check [--tighten]   # exit 1 if the count rose; --tighten lowers the baseline
  py -3 tools/journal_comment_ratchet.py --write-baseline      # re-record (say why in the commit)
  py -3 tools/journal_comment_ratchet.py --diff BASE [--staged]

Waiver: a line carrying `journal-comment: allow` is not counted (a real identifier that matches a pattern).
Exit: 0 ok, 1 count rose / new journal comment, 2 usage or baseline error. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BASELINE = os.path.join(HERE, "journal_comment_ratchet.baseline.json")
EXTS = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl")
EXCLUDE_PARTS = ("3rdparty/", "graphify-out/", "/Tracy/", "build64", "/__pycache__/")
WAIVER = "journal-comment: allow"

# Each pattern is matched against comment text only (never code or string literals).
PATTERNS = {
    "batch-tag": re.compile(r"\bB\d\.\d+[a-z]?(?:-pre)?\b|\bB\d-pre\b"),
    "step-slice-phase": re.compile(r"\b(?:[Ss]tep|[Ss]lice|[Pp]hase|[Ww]ave|[Bb]atch)\s+(?:\d+[a-z]?|[A-H])\b(?![.\d]*\s*(?:ms|px|%|bytes?|frames?))"),
    "plan-doc": re.compile(r"\.claude/|[\w-]+-(?:notes|plan|review|design|audit|state|ledger)\.md\b|\bnotes section\b|\bBUGS(?:-FOUND-FIXED)? row\b|\bHANDOFF\b"),
    "review-id": re.compile(r"\b(?:RB\d|RA\d)\b|\breview (?:item |finding |fix )?[A-Z]{1,2}\d{1,2}\b|\(\s*[DHRF]\d{1,2}\s*\)"),
    "arc-id": re.compile(r"\b[A-Z][A-Z0-9]{2,}(?:-[A-Z0-9]+)*-\d+[a-z]?\b(?!-)"),
    "agent-name":re.compile(r"\b(?:Sonnet|Opus|Fable|Haiku|Codex|Claude)\b|\b[Ll]ane\s?[1-5]\b"),
}

_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def comment_lines(text: str) -> dict[int, str]:
    """Map 1-based line number -> comment text on that line (string literals are skipped)."""
    out: dict[int, str] = {}
    for m in _TOKEN.finditer(text):
        tok = m.group(0)
        if tok[0] in "\"'":
            continue
        line = text.count("\n", 0, m.start()) + 1
        for i, part in enumerate(tok.split("\n")):
            out[line + i] = (out.get(line + i, "") + " " + part).strip()
    return out


# Standard names shaped like an arc id (hash and encoding families, the smoke tiers).
NOT_ARC_IDS = ("FNV", "UTF", "SHA", "ISO", "CRC", "RFC", "IEEE", "TIER", "UTC")


def hits_in_comment(comment: str) -> list[str]:
    """Names of the journal patterns the comment text matches."""
    out = []
    for name, rx in PATTERNS.items():
        ms = rx.finditer(comment)
        if name == "arc-id":
            ms = (m for m in ms if m.group(0).split("-")[0] not in NOT_ARC_IDS)
        if next(ms, None) is not None:
            out.append(name)
    return out


def scan_text(text: str) -> list[tuple[int, str, str]]:
    """(line, pattern, comment) for every journal comment in one source text."""
    lines = text.split("\n")
    out = []
    for ln, comment in sorted(comment_lines(text).items()):
        if WAIVER in (lines[ln - 1] if ln - 1 < len(lines) else ""):
            continue
        for name in hits_in_comment(comment):
            out.append((ln, name, comment))
    return out


def wanted(path: str) -> bool:
    p = "/" + path.replace("\\", "/")
    return p.endswith(EXTS) and not any(x in p for x in EXCLUDE_PARTS)


def git(*args: str, root: str = ROOT) -> str:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", check=True, **console_child.child_kwargs()).stdout


def scan_tree(rev: str, root: str = ROOT) -> list[tuple[str, int, str, str]]:
    files = [f for f in git("ls-tree", "-r", "--name-only", rev, root=root).splitlines() if wanted(f)]
    blobs = subprocess.run(["git", "cat-file", "--batch"], cwd=root, check=True, capture_output=True,
                           input="".join(f"{rev}:{f}\n" for f in files).encode("utf-8"),
                           **console_child.child_kwargs()).stdout
    out, pos = [], 0
    for f in files:
        nl = blobs.index(b"\n", pos)
        size = int(blobs[pos:nl].split()[2])
        text = blobs[nl + 1:nl + 1 + size].decode("utf-8", errors="replace")
        pos = nl + 1 + size + 1
        out += [(f, ln, name, c) for ln, name, c in scan_text(text)]
    return out


def added_lines(base: str, staged: bool, root: str = ROOT) -> dict[str, set[int]]:
    """Per file, the 1-based line numbers added since BASE (HEAD side, or the index with --staged)."""
    args = ["diff", "--unified=0", "--no-color"] + (["--cached", base] if staged else [f"{base}..HEAD"])
    out: dict[str, set[int]] = {}
    cur = None
    for line in git(*args, root=root).splitlines():
        if line.startswith("+++ "):
            cur = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and cur and wanted(cur):
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            start, n = int(m.group(1)), int(m.group(2) or 1)
            out.setdefault(cur, set()).update(range(start, start + n))
    return out


def check_diff(base: str, staged: bool, root: str = ROOT) -> list[tuple[str, int, str, str]]:
    bad = []
    for f, lines in sorted(added_lines(base, staged, root).items()):
        text = git("show", f"{':' if staged else 'HEAD:'}{f}", root=root)
        bad += [(f, ln, name, c) for ln, name, c in scan_text(text) if ln in lines]
    return bad


def load_baseline(path: str = BASELINE) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def write_baseline(hits, rev: str, path: str = BASELINE) -> None:
    data = {"what": "count of journal comments in C++ comments; may only fall (tools/journal_comment_ratchet.py)",
            "rev": rev, "total": len(hits), "by_pattern": dict(sorted(Counter(h[2] for h in hits).items()))}
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--tighten", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--diff", metavar="BASE")
    ap.add_argument("--staged", action="store_true")
    a = ap.parse_args(argv)

    if a.diff:
        bad = check_diff(a.diff, a.staged)
        for f, ln, name, c in bad:
            print(f"{f}:{ln}: {name}: {c[:160]}")
        print(f"journal_comment_ratchet: {'FAIL' if bad else 'OK'} - {len(bad)} journal comment(s) on added lines since {a.diff}")
        return 1 if bad else 0

    rev = git("rev-parse", "--short", a.rev).strip()
    hits = scan_tree(a.rev)
    if a.write_baseline:
        write_baseline(hits, rev)
        print(f"journal_comment_ratchet: baseline {len(hits)} at {rev}")
        return 0
    if a.list:
        for f, ln, name, c in hits:
            print(f"{f}:{ln}: {name}: {c[:160]}")
    if not a.check:
        for name, n in Counter(h[2] for h in hits).most_common():
            print(f"{name:18} {n}")
        for f, n in Counter(h[0] for h in hits).most_common(20):
            print(f"  {n:5} {f}")
        print(f"journal_comment_ratchet: {len(hits)} at {rev}")
        return 0
    try:
        base = load_baseline()
    except (OSError, ValueError) as e:
        print(f"journal_comment_ratchet: baseline unreadable: {e}", file=sys.stderr)
        return 2
    if len(hits) > base["total"]:
        print(f"journal_comment_ratchet: FAIL - {len(hits)} > baseline {base['total']} (run --diff <base> to see the new ones)")
        return 1
    if a.tighten and len(hits) < base["total"]:
        write_baseline(hits, rev)
    print(f"journal_comment_ratchet: OK - {len(hits)} <= baseline {base['total']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
