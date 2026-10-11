#!/usr/bin/env python3
"""apply_edit - exact search/replace edits read from a JSON file.

WHAT: applies `[{"file", "search", "replace", "count": 1}]` to the named files, prints a unified diff, and writes nothing unless every edit matched.
WHY: a replacement typed into a shell heredoc loses its backslashes (`\\n`, `\\\\`) and quotes; a JSON file written with the Write tool carries the
text byte for byte. Refusing on a count mismatch keeps a stale or ambiguous anchor from editing the wrong place.

  py -3 tools/dev/apply_edit.py edits.json            # apply, print the diff
  py -3 tools/dev/apply_edit.py edits.json --check    # dry run: same checks and diff, no write

Rules:
  - `search` is matched literally (no regex) and must occur exactly `count` times (default 1) in the file as edited so far; edits to one file apply in order.
  - Paths are relative to the repository root unless absolute.
  - A CRLF file is matched and written with CRLF: `\\n` in search/replace means the file's own line ending.
  - All or nothing: one refused edit leaves every file untouched. Exit 0 = applied (or would apply), 1 = refused, 2 = bad input.
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

#: Bytes that are not valid UTF-8 (legacy CP1252 strings) survive the round trip as lone surrogates.
_ENC = {"encoding": "utf-8", "errors": "surrogateescape"}


class EditError(Exception):
    """An edit that cannot be applied exactly; carries the message printed to the user."""


def load_edits(path: str) -> list[dict]:
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc
    if isinstance(data, dict) and "edits" in data:
        data = data["edits"]
    if not isinstance(data, list) or not data:
        raise ValueError("the JSON must be a non-empty list of edits (or {\"edits\": [...]})")
    for i, e in enumerate(data):
        if not isinstance(e, dict):
            raise ValueError(f"edit {i}: not an object")
        for key in ("file", "search", "replace"):
            if not isinstance(e.get(key), str):
                raise ValueError(f"edit {i}: '{key}' must be a string")
        count = e.get("count", 1)
        if not isinstance(count, int) or isinstance(count, bool) or count < 1:
            raise ValueError(f"edit {i}: 'count' must be an integer >= 1")
        unknown = set(e) - {"file", "search", "replace", "count"}
        if unknown:
            raise ValueError(f"edit {i}: unknown key(s) {sorted(unknown)}")
    return data


def _resolve(file: str, root: str) -> str:
    return file if os.path.isabs(file) else os.path.join(root, file)


def _eol_of(text: str) -> str:
    """The file's line ending: CRLF when it has one and no bare LF, else LF."""
    crlf = text.count("\r\n")
    return "\r\n" if crlf and crlf == text.count("\n") else "\n"


def _in_eol(s: str, eol: str) -> str:
    if eol == "\n" or "\r\n" in s:
        return s
    return s.replace("\n", eol)


def plan(edits: list[dict], root: str) -> dict[str, tuple[str, str]]:
    """Apply every edit in memory. Returns {path: (before, after)}; raises EditError on the first mismatch."""
    state: dict[str, tuple[str, str]] = {}
    for i, e in enumerate(edits):
        path = _resolve(e["file"], root)
        if path not in state:
            if not os.path.isfile(path):
                raise EditError(f"edit {i}: {e['file']}: no such file")
            with open(path, "rb") as fh:
                text = fh.read().decode(**_ENC)
            state[path] = (text, text)
        before, cur = state[path]
        eol = _eol_of(before)
        search, replace = _in_eol(e["search"], eol), _in_eol(e["replace"], eol)
        if not search:
            raise EditError(f"edit {i}: {e['file']}: empty search")
        want = e.get("count", 1)
        got = cur.count(search)
        if got != want:
            head = search.strip().splitlines()[0][:80] if search.strip() else repr(search[:40])
            raise EditError(f"edit {i}: {e['file']}: search found {got} time(s), expected {want}: {head!r}")
        state[path] = (before, cur.replace(search, replace))
    return state


def diff_of(path: str, before: str, after: str, root: str) -> str:
    rel = os.path.relpath(path, root).replace("\\", "/")
    return "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True), f"a/{rel}", f"b/{rel}"))


def write_all(state: dict[str, tuple[str, str]]) -> None:
    for path, (before, after) in state.items():
        if before == after:
            continue
        tmp = path + ".apply_edit.tmp"
        with open(tmp, "wb") as fh:
            fh.write(after.encode(**_ENC))
        os.replace(tmp, path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Apply exact search/replace edits from a JSON file.")
    ap.add_argument("edits", help="JSON file: [{\"file\",\"search\",\"replace\",\"count\":1}]")
    ap.add_argument("--check", action="store_true", help="dry run: verify every edit and print the diff, write nothing")
    ap.add_argument("--root", default=ROOT, help="base for relative paths (default: the repository root)")
    args = ap.parse_args(argv)
    try:
        edits = load_edits(args.edits)
    except ValueError as exc:
        print(f"apply_edit: {exc}", file=sys.stderr)
        return 2
    try:
        state = plan(edits, args.root)
    except EditError as exc:
        print(f"apply_edit: REFUSED, nothing written: {exc}", file=sys.stderr)
        return 1
    changed = 0
    for path, (before, after) in state.items():
        d = diff_of(path, before, after, args.root)
        if d:
            changed += 1
            sys.stdout.write(d if d.endswith("\n") else d + "\n")
        else:
            print(f"apply_edit: {os.path.relpath(path, args.root)}: no change (replace equals search)")
    if not args.check:
        write_all(state)
    verb = "would apply" if args.check else "applied"
    print(f"apply_edit: {verb} {len(edits)} edit(s) to {changed} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
