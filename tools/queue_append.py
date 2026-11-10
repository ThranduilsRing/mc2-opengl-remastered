#!/usr/bin/env python3
"""Append one real-clock-stamped entry to the orchestrator queue file.

  py -3 tools/queue_append.py "text"      (or --file <path>, or text on stdin)
  py -3 tools/queue_append.py --check     (read-only: list out-of-order or future stamps)

Entry format: `[YYYY-MM-DD HH:MM] text`. Default queue A:/Games/mc2-ops/W135-queue.txt,
overridable with --queue or MC2_QUEUE_FILE.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import sys
from pathlib import Path

DEFAULT_QUEUE = "A:/Games/mc2-ops/W135-queue.txt"
STAMP_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2})\]")
STAMP_FMT = "%Y-%m-%d %H:%M"


def now() -> dt.datetime:
    return dt.datetime.now()


def format_entry(text: str, clock=now, keep_newlines: bool = False) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n").strip("\n")
    if not keep_newlines:
        text = " | ".join(p.strip() for p in text.split("\n") if p.strip())
    return f"[{clock().strftime(STAMP_FMT)}] {text}\n"


def append_entry(queue: Path, entry: str) -> None:
    data = entry.encode("utf-8")
    with open(queue, "ab") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())


def check_queue(queue: Path, clock=now) -> list[str]:
    """Return problem lines; entries are lines starting with a stamp."""
    stamps = []
    for lineno, line in enumerate(queue.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        m = STAMP_RE.match(line)
        if m:
            stamps.append((lineno, dt.datetime.strptime(m.group(1), STAMP_FMT), line))
    limit = clock()
    out = []
    for i, (lineno, ts, line) in enumerate(stamps):
        reasons = []
        if ts > limit:
            reasons.append("later than now")
        if i + 1 < len(stamps) and ts > stamps[i + 1][1]:
            reasons.append(f"later than next entry (line {stamps[i + 1][0]}, {stamps[i + 1][1].strftime(STAMP_FMT)})")
        if reasons:
            out.append(f"line {lineno}: {'; '.join(reasons)}: {line[:100]}")
    return out


def main(argv=None, clock=now, stdin=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", nargs="?", help="entry text (omit to read --file or stdin)")
    ap.add_argument("--file", help="read entry text from this file")
    ap.add_argument("--queue", default=os.environ.get("MC2_QUEUE_FILE") or DEFAULT_QUEUE)
    ap.add_argument("--keep-newlines", action="store_true", help="keep embedded newlines instead of ' | '")
    ap.add_argument("--check", action="store_true", help="read-only scan for bad stamps")
    args = ap.parse_args(argv)
    queue = Path(args.queue)
    if args.check:
        bad = check_queue(queue, clock)
        for line in bad:
            print(line)
        print(f"queue_append --check: {len(bad)} suspect entr{'y' if len(bad) == 1 else 'ies'} in {queue}")
        return 1 if bad else 0
    if args.text is not None:
        text = args.text
    elif args.file:
        text = Path(args.file).read_text(encoding="utf-8")
    else:
        text = (stdin or sys.stdin).read()
    if not text.strip():
        print("queue_append: empty entry", file=sys.stderr)
        return 2
    entry = format_entry(text, clock, args.keep_newlines)
    append_entry(queue, entry)
    print(entry.rstrip("\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
