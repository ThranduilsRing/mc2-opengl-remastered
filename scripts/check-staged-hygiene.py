#!/usr/bin/env python3
"""Reject EOL drift and new control bytes without reading working files.

Hook command: py -3 scripts/check-staged-hygiene.py [--index] [--quiet]
Board command: py -3 scripts/check-staged-hygiene.py --range [BASE..TIP]
Exit codes: 0 clean, 1 hygiene findings, 2 Git/input error.

INDEX mode compares HEAD with the index. Range mode compares committed trees;
its default is merge-base(claude/nifty-mendeleev)..HEAD on task branches, or
HEAD~1..HEAD on that branch itself, so the board checks committed work even
with an empty index diff. Attributes come from the destination tree or index.
The baseline and destination blobs establish style; eol attributes
describe checkout policy, so eol=crlf does not require CRLF in a normalized blob.
Explicit binary files, heuristic binary files
and UTF-16/32 BOM files are skipped. A NUL added to previously NUL-free text is
a finding, not a reason to reclassify that file as binary. Existing controls
are grandfathered only at the identical zero-based byte offset and value.
An old UTF-16/32 BOM does not exempt a new narrow-text blob or establish style.
Existing mixed endings are tolerated; uniform committed endings must survive.
New files and newline-free baselines may gain either uniform style, never mixed
endings. Symlinks and submodules are skipped.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

import console_child


CONTROLS = frozenset(range(9)) | {11, 12} | frozenset(range(14, 32)) | {127}
WIDE_BOMS = (b"\xff\xfe", b"\xfe\xff", b"\x00\x00\xfe\xff")


def git(repo: Path, *args: str, data: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["git", *args], cwd=repo, input=data, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=30,
        **console_child.child_kwargs(redirected=True),
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"git {' '.join(args[:2])}: {detail}")
    return result.stdout


def revision_range(repo: Path, value: str) -> tuple[str, str]:
    if value == "auto":
        branch = git(repo, "rev-parse", "--abbrev-ref", "HEAD").strip()
        base = "HEAD~1" if branch == b"claude/nifty-mendeleev" else git(
            repo, "merge-base", "claude/nifty-mendeleev", "HEAD",
        ).decode("ascii").strip()
        tip = "HEAD"
    else:
        if value.count("..") != 1 or "..." in value:
            raise ValueError("--range requires BASE..TIP (two committed endpoints)")
        base, tip = value.split("..")
        if not base or not tip:
            raise ValueError("--range requires nonempty BASE and TIP")
    return tuple(
        git(repo, "rev-parse", "--verify", "--end-of-options", f"{rev}^{{commit}}")
        .decode("ascii").strip() for rev in (base, tip)
    )


def changed_files(repo: Path, revisions: tuple[str, str] | None) -> list[tuple[str, str, str]]:
    """Return destination path and both blob ids for changed regular files."""
    # Raw records preserve rename ancestry and exclude non-file objects from
    # blob reads; both modes assess the same additions and modifications.
    records = git(
        repo, "diff", "--raw", "-z", "--no-abbrev",
        "--no-ext-diff", "--no-textconv", "--find-renames", "--diff-filter=ACMRT",
        *(revisions if revisions is not None else ("--cached",)), "--",
    ).split(b"\0")
    files = []
    pos = 0
    while pos < len(records) and records[pos]:
        old_mode, new_mode, old_id, new_id, status = records[pos].split()
        path = records[pos + 1]
        pos += 2
        if status[:1] in (b"R", b"C"):
            path = records[pos]
            pos += 1
        if new_mode not in (b"100644", b"100755"):
            continue
        if old_mode[1:] not in (b"100644", b"100755"):
            old_id = b"0" * len(old_id)
        files.append((os.fsdecode(path), old_id.decode("ascii"), new_id.decode("ascii")))
    return files


def destination_attributes(repo: Path, paths: list[str], revision: str | None) -> dict[str, dict[str, str]]:
    if not paths:
        return {}
    raw = git(
        repo, "check-attr", *(('--source', revision) if revision else ('--cached',)),
        "-z", "--stdin", "text", "eol", "binary",
        data=b"".join(os.fsencode(path) + b"\0" for path in paths),
    ).split(b"\0")
    attrs: dict[str, dict[str, str]] = {}
    for pos in range(0, len(raw) - 1, 3):
        path, attr, value = raw[pos:pos + 3]
        attrs.setdefault(os.fsdecode(path), {})[attr.decode("ascii")] = value.decode("ascii")
    return attrs


def read_blobs(repo: Path, files: list[tuple[str, str, str]]) -> dict[str, bytes]:
    ids = list(dict.fromkeys(blob for _, old, new in files for blob in (old, new) if blob.strip("0")))
    if not ids:
        return {}
    raw = git(repo, "cat-file", "--batch", data="".join(f"{blob}\n" for blob in ids).encode("ascii"))
    blobs = {}
    pos = 0
    for blob in ids:
        end = raw.find(b"\n", pos)
        header = raw[pos:end].split() if end >= 0 else []
        if len(header) != 3 or header[0] != blob.encode("ascii") or header[1] != b"blob":
            raise ValueError(f"invalid cat-file batch header for {blob}")
        size = int(header[2])
        start = end + 1
        pos = start + size
        if size < 0 or raw[pos:pos + 1] != b"\n":
            raise ValueError(f"incomplete cat-file batch blob {blob}")
        blobs[blob] = raw[start:pos]
        pos += 1
    if pos != len(raw):
        raise ValueError("unexpected trailing cat-file batch output")
    return blobs


def skip_reason(before: bytes | None, after: bytes, attrs: dict[str, str]) -> str | None:
    if attrs.get("binary") == "set" or attrs.get("text") == "unset":
        return "binary attribute"
    if after.startswith(WIDE_BOMS):
        return "UTF-16/32 BOM"
    forced_text = attrs.get("text") == "set" or (
        attrs.get("eol") in ("lf", "crlf") and attrs.get("text") != "auto"
    )
    if not forced_text and b"\0" in (before if before is not None else after):
        return "NUL binary heuristic"
    return None


def eol_style(data: bytes) -> str | None:
    lf = data.count(b"\n")
    crlf = data.count(b"\r\n")
    if crlf and lf != crlf:
        return "mixed"
    return "CRLF" if crlf else "LF" if lf else None


def findings(before: bytes | None, after: bytes, labels: tuple[str, str] = ("HEAD", "INDEX")) -> list[str]:
    problems = []
    old_style = eol_style(before) if before is not None else None
    new_style = eol_style(after)
    if old_style in ("LF", "CRLF") and new_style not in (None, old_style):
        problems.append(f"EOL drift: {labels[0]} {old_style} -> {labels[1]} {new_style}")
    elif old_style is None and new_style == "mixed":
        problems.append("mixed LF/CRLF endings without a mixed baseline")
    new_controls = [
        (offset, value) for offset, value in enumerate(after)
        if value in CONTROLS
        and (before is None or offset >= len(before) or before[offset] != value)
    ]
    if new_controls:
        detail = ", ".join(f"0x{value:02X} at byte {offset}" for offset, value in new_controls[:8])
        problems.append(f"{len(new_controls)} new control byte(s): {detail}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="Git worktree (default: cwd)")
    parser.add_argument("--quiet", action="store_true", help="suppress clean summary")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--index", action="store_true", help="compare HEAD to INDEX (default; hooks)")
    mode.add_argument("--range", nargs="?", const="auto", metavar="BASE..TIP",
                      help="compare committed trees (default range: see module description)")
    args = parser.parse_args(argv)
    try:
        repo = Path(os.fsdecode(git(args.repo, "rev-parse", "--show-toplevel")).strip())
        revisions = revision_range(repo, args.range) if args.range is not None else None
        scope = f"RANGE {revisions[0]}..{revisions[1]}" if revisions else "INDEX (HEAD..INDEX)"
        files = changed_files(repo, revisions)
        attrs = destination_attributes(repo, [path for path, _, _ in files], revisions[1] if revisions else None)
        blobs = read_blobs(repo, files)
        checked = skipped = failed = 0
        for path, old_id, new_id in files:
            before = blobs.get(old_id)
            after = blobs[new_id]
            if before is not None and before.startswith(WIDE_BOMS):
                before = None
            if skip_reason(before, after, attrs.get(path, {})):
                skipped += 1
                continue
            checked += 1
            issues = findings(before, after, ("BASE", "TIP") if revisions else ("HEAD", "INDEX"))
            if issues:
                failed += 1
                print(f"FAIL {path!r}: {'; '.join(issues)}")
        if not args.quiet or failed:
            print(f"staged-hygiene: {scope}: {checked} text checked, {skipped} skipped, {failed} failed")
        return 1 if failed else 0
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"staged-hygiene: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
