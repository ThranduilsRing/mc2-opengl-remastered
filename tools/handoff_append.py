#!/usr/bin/env python3
"""Edit `.claude/cleanup/ORCHESTRATOR-HANDOFF.md` after a milestone and commit just that file.

WHY: the orchestrator records every milestone in the handoff doc (lane status, new
state bullets, queue-item annotations). Doing that by hand or through a model agent is
slow and error-prone; this applies the same edits deterministically and commits only
the doc, so a concurrent lane's staged work is never swept in.

Usage (all ops optional, at least one required):
  py -3 tools/handoff_append.py --lanes "<text>"               replace the `- Lanes (` bullet in "## State at handoff"
                                                               (fresh `YYYY-MM-DD HH:MM` local timestamp)
  py -3 tools/handoff_append.py --bullet "<text>" [--bullet ..] append `- <text>` at the end of "## State at handoff"
  py -3 tools/handoff_append.py --queue-prefix N "<text>"      prepend text right after item N's number
  py -3 tools/handoff_append.py --queue-replace N "<old>" "<new>"  exact substring replace inside item N
  py -3 tools/handoff_append.py --queue-append "<text>"        new numbered item at the end of "## Ordered queue"
  --message "<subject>"   commit subject (default: "handoff: update ORCHESTRATOR-HANDOFF")
  --dry-run               print the diff, change nothing
  --root DIR / --file REL repo root and doc path (defaults: this checkout / the real handoff)

Applied in the order lanes, bullets, queue-prefix, queue-replace, queue-append.
Only the named sections change; line-1 header and LF endings are preserved.

Exit codes: 0 ok; 2 bad request (section/item missing, text absent or ambiguous, prefix
already present, nothing to do); 3 merge in progress / unmerged paths / doc has
uncommitted edits (refused); 4 git failure.
"""
from __future__ import annotations

import argparse
import datetime
import difflib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each)
sys.path.insert(0, HERE)
import _git_util  # noqa: E402  (shared git helpers)

DEFAULT_FILE = ".claude/cleanup/ORCHESTRATOR-HANDOFF.md"
TRAILER = "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
STATE_HEADING = "## State at handoff"
QUEUE_HEADING = "## Ordered queue"
UNMERGED = _git_util.UNMERGED
ITEM_RE = re.compile(r"^(\d+)\. ")


class Refusal(Exception):
    def __init__(self, code: int, msg: str):
        super().__init__(msg)
        self.code = code


def git(root: str, *args: str) -> subprocess.CompletedProcess:
    return _git_util.git(root, *args)


def check_repo_state(root: str, rel: str) -> None:
    """Refuse while a merge is open, paths are unmerged, or the doc carries someone else's edits."""
    r = git(root, "rev-parse", "--git-path", "MERGE_HEAD")
    if r.returncode != 0:
        raise Refusal(4, "not a git repo: " + r.stderr.strip())
    merge_head = r.stdout.strip()
    if not os.path.isabs(merge_head):
        merge_head = os.path.join(root, merge_head)
    if os.path.exists(merge_head):
        raise Refusal(3, "merge in progress (MERGE_HEAD exists); finish or abort it first")
    st = git(root, "status", "--porcelain")
    if st.returncode != 0:
        raise Refusal(4, "git status failed: " + st.stderr.strip())
    unmerged = [ln for ln in st.stdout.splitlines() if ln[:2] in UNMERGED]
    if unmerged:
        raise Refusal(3, "unmerged paths present:\n  " + "\n  ".join(unmerged))
    dirty = git(root, "status", "--porcelain", "--", rel).stdout.strip()
    if dirty:
        raise Refusal(3, f"{rel} has uncommitted edits; committing it would sweep them in: {dirty}")


def section_bounds(lines: list[str], heading: str) -> tuple[int, int]:
    """(first body line, line index of the next `## ` heading or EOF) for a section."""
    start = next((i for i, ln in enumerate(lines) if ln.startswith(heading)), None)
    if start is None:
        raise Refusal(2, f'section "{heading}" not found')
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    return start + 1, end


def last_content_line(lines: list[str], lo: int, hi: int) -> int:
    """Index of the last non-blank line in [lo, hi), or lo-1 when the range is blank."""
    i = hi - 1
    while i >= lo and not lines[i].strip():
        i -= 1
    return i


def one_line(text: str, what: str) -> str:
    if "\n" in text or "\r" in text:
        raise Refusal(2, f"{what} must be a single line")
    return text


def item_span(lines: list[str], n: int) -> tuple[int, int]:
    """[start, end) lines of queue item n: its numbered line plus continuation lines up to a blank/next item."""
    lo, hi = section_bounds(lines, QUEUE_HEADING)
    start = None
    for i in range(lo, hi):
        m = ITEM_RE.match(lines[i])
        if m and int(m.group(1)) == n:
            start = i
            break
    if start is None:
        raise Refusal(2, f"queue item {n} not found")
    end = start + 1
    while end < hi and lines[end].strip() and not ITEM_RE.match(lines[end]):
        end += 1
    return start, end


def op_lanes(lines: list[str], text: str, now: datetime.datetime) -> None:
    lo, hi = section_bounds(lines, STATE_HEADING)
    idx = next((i for i in range(lo, hi) if lines[i].startswith("- Lanes (")), None)
    if idx is None:
        raise Refusal(2, 'no "- Lanes (" bullet in "## State at handoff"')
    lines[idx] = f"- Lanes ({now.strftime('%Y-%m-%d %H:%M')}): {one_line(text, '--lanes')}"


def op_bullet(lines: list[str], text: str) -> None:
    lo, hi = section_bounds(lines, STATE_HEADING)
    last = last_content_line(lines, lo, hi)
    lines.insert(last + 1, f"- {one_line(text, '--bullet')}")


def op_queue_prefix(lines: list[str], n: int, text: str) -> None:
    start, _ = item_span(lines, n)
    m = ITEM_RE.match(lines[start])
    rest = lines[start][m.end():]
    text = one_line(text, "--queue-prefix")
    if rest.startswith(text):
        raise Refusal(2, f"queue item {n} already starts with that prefix")
    lines[start] = f"{n}. {text} {rest}"


def op_queue_replace(lines: list[str], n: int, old: str, new: str) -> None:
    start, end = item_span(lines, n)
    body = "\n".join(lines[start:end])
    count = body.count(old) if old else 0
    if count == 0:
        raise Refusal(2, f"text not found in queue item {n}")
    if count > 1:
        raise Refusal(2, f"text occurs {count} times in queue item {n}; give a unique substring")
    lines[start:end] = body.replace(old, new, 1).split("\n")


def op_queue_append(lines: list[str], text: str) -> None:
    lo, hi = section_bounds(lines, QUEUE_HEADING)
    nums = [int(m.group(1)) for i in range(lo, hi) if (m := ITEM_RE.match(lines[i]))]
    if not nums:
        raise Refusal(2, "no numbered items in the queue")
    _, last_end = item_span(lines, nums[-1])
    lines.insert(last_end, f"{nums[-1] + 1}. {one_line(text, '--queue-append')}")


def apply_ops(text: str, args: argparse.Namespace, now: datetime.datetime) -> str:
    lines = text.split("\n")
    if args.lanes is not None:
        op_lanes(lines, args.lanes, now)
    for b in args.bullet:
        op_bullet(lines, b)
    for n, t in args.queue_prefix:
        op_queue_prefix(lines, int(n), t)
    for n, old, new in args.queue_replace:
        op_queue_replace(lines, int(n), old, new)
    for t in args.queue_append:
        op_queue_append(lines, t)
    return "\n".join(lines)


def short_diff(old: str, new: str, width: int = 220) -> str:
    def clip(ln: str) -> str:
        return ln if len(ln) <= width else ln[:width] + f"...[+{len(ln) - width} chars]"
    d = difflib.unified_diff(old.split("\n"), new.split("\n"), "a/handoff", "b/handoff", lineterm="", n=0)
    return "\n".join(clip(ln) for ln in d)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lanes")
    ap.add_argument("--bullet", action="append", default=[])
    ap.add_argument("--queue-prefix", nargs=2, action="append", default=[], metavar=("N", "TEXT"))
    ap.add_argument("--queue-replace", nargs=3, action="append", default=[], metavar=("N", "OLD", "NEW"))
    ap.add_argument("--queue-append", action="append", default=[])
    ap.add_argument("--message", default="handoff: update ORCHESTRATOR-HANDOFF")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--root", default=os.path.abspath(os.path.join(HERE, "..")))
    ap.add_argument("--file", default=DEFAULT_FILE)
    return ap


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")  # the doc holds non-cp1252 characters
    args = build_parser().parse_args(argv)
    try:
        if args.lanes is None and not (args.bullet or args.queue_prefix or args.queue_replace or args.queue_append):
            raise Refusal(2, "no operation given")
        for n, *_ in list(args.queue_prefix) + list(args.queue_replace):
            if not n.isdigit():
                raise Refusal(2, f"item number must be an integer, got {n!r}")
        check_repo_state(args.root, args.file)
        path = os.path.join(args.root, args.file)
        with open(path, "rb") as f:
            old = f.read().decode("utf-8")
        new = apply_ops(old, args, datetime.datetime.now())
        if args.dry_run:
            print(short_diff(old, new))
            print("[dry-run] nothing written, nothing committed")
            return 0
        if new == old:
            raise Refusal(2, "edit produced no change")
        with open(path, "wb") as f:
            f.write(new.encode("utf-8"))
        r = git(args.root, "add", "--", args.file)
        if r.returncode == 0:
            r = git(args.root, "commit", "-m", args.message, "-m", TRAILER, "--", args.file)
        if r.returncode != 0:
            raise Refusal(4, "git commit failed: " + (r.stderr or r.stdout).strip())
        print(git(args.root, "rev-parse", "HEAD").stdout.strip())
        return 0
    except Refusal as e:
        print(f"handoff_append: {e}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
