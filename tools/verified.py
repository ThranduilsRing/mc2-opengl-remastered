#!/usr/bin/env python3
"""Append and query evidence for canonical Git commits.

Exit 0: evidence returned; 1: no evidence; 2: invalid input/persistence failure.
Report IDs are recorder assertions; this tool does not execute or grade proofs.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts import console_child

FIELDS = ("build", "doctest", "board", "tier1", "tier2", "tier3", "captures", "tracy_baseline")
DEFAULT_LEDGER = ROOT / "tests" / "verified.json"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def resolve_commit(revision: str, root: Path = ROOT) -> str:
    """Reject missing/ambiguous revisions and peel tags to a full commit SHA."""
    if not isinstance(revision, str) or not revision.strip() or "\0" in revision:
        raise ValueError("revision must name a Git commit")
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "--end-of-options", revision + "^{commit}"],
        capture_output=True, text=True, **console_child.child_kwargs(redirected=True),
    )
    sha = result.stdout.strip()
    if result.returncode or not SHA_RE.fullmatch(sha):
        raise ValueError(f"unknown or ambiguous commit: {revision}")
    return sha


def empty_ledger() -> dict[str, Any]:
    return {"schema_version": 1, "records": []}


def _validate_fields(fields: dict[str, Any]) -> None:
    for field, value in fields.items():
        if field not in FIELDS:
            raise ValueError(f"unknown proof field: {field}")
        if field == "captures":
            if not isinstance(value, list) or any(not isinstance(x, str) or not x.strip() for x in value):
                raise ValueError("captures must be a list of nonempty strings")
            if len(set(value)) != len(value):
                raise ValueError("captures must not contain duplicate IDs")
        elif value is not None and (not isinstance(value, str) or not value.strip()):
            raise ValueError(f"{field} must be a nonempty string or null")


def validate_ledger(value: Any) -> None:
    if (not isinstance(value, dict) or set(value) != {"schema_version", "records"}
            or type(value["schema_version"]) is not int or value["schema_version"] != 1
            or not isinstance(value["records"], list)):
        raise ValueError("unsupported verified ledger schema")
    seen: set[str] = set()
    for row in value["records"]:
        if not isinstance(row, dict) or set(row) != {"sha", *FIELDS}:
            raise ValueError("malformed verified ledger record fields")
        sha = row["sha"]
        if not isinstance(sha, str) or not SHA_RE.fullmatch(sha) or sha in seen:
            raise ValueError("ledger SHAs must be unique canonical full commit SHAs")
        seen.add(sha)
        _validate_fields({field: row[field] for field in FIELDS})


def read_ledger(path: Path) -> dict[str, Any]:
    if not path.exists():
        return empty_ledger()
    value = json.loads(path.read_text(encoding="utf-8"))
    validate_ledger(value)
    return value


@contextmanager
def ledger_lock(path: Path, timeout: float = 30):
    """A persistent lock inode protects the entire read/merge/replace transaction."""
    path.parent.mkdir(parents=True, exist_ok=True)
    # Windows permits locking beyond EOF. Never initialize the byte before
    # locking: another writer may already own it when a buffered write flushes.
    with path.with_name(path.name + ".lock").open("a+b", buffering=0) as lock:
        deadline = time.monotonic() + timeout
        while True:
            try:
                lock.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise OSError(f"timed out acquiring ledger lock: {path}")
                time.sleep(0.02)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == "nt":
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def _write_ledger(path: Path, ledger: dict[str, Any]) -> None:
    validate_ledger(ledger)
    temporary: str | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=path.parent, prefix=path.name + ".", suffix=".tmp",
                                         delete=False) as stream:
            temporary = stream.name
            stream.write(json.dumps(ledger, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary and os.path.exists(temporary):
            os.unlink(temporary)


def _canonical(revision: str, resolver: Callable[[str], str]) -> str:
    sha = resolver(revision)
    if not isinstance(sha, str) or not SHA_RE.fullmatch(sha):
        raise ValueError("resolver must return a canonical full commit SHA")
    return sha


def record_proof(path: Path, sha: str, updates: dict[str, Any], *,
                 resolver: Callable[[str], str] = resolve_commit) -> dict[str, Any]:
    if not isinstance(updates, dict) or not updates:
        raise ValueError("record requires at least one proof field")
    _validate_fields(updates)
    canonical = _canonical(sha, resolver)
    with ledger_lock(path):
        ledger = read_ledger(path)
        previous = next((row for row in ledger["records"] if row["sha"] == canonical), None)
        merged = {"sha": canonical, **{field: None for field in FIELDS}, "captures": []}
        if previous:
            merged.update(previous)
            merged["captures"] = list(previous["captures"])
            ledger["records"].remove(previous)
        for field, value in updates.items():
            if field == "captures":
                merged[field] = list(dict.fromkeys(merged[field] + value))
            else:
                merged[field] = value
        ledger["records"].append(merged)
        _write_ledger(path, ledger)
        return merged


def show_proof(path: Path, sha: str, *, resolver: Callable[[str], str] = resolve_commit) -> dict[str, Any] | None:
    canonical = _canonical(sha, resolver)
    return next((row for row in read_ledger(path)["records"] if row["sha"] == canonical), None)


def latest_proof(path: Path) -> dict[str, Any] | None:
    records = read_ledger(path)["records"]
    return records[-1] if records else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    record = sub.add_parser("record", help="merge proof IDs; most recent write becomes latest")
    record.add_argument("sha")
    for field in FIELDS:
        if field != "captures":
            record.add_argument("--" + field.replace("_", "-"))
    record.add_argument("--capture", action="append", default=[])
    show = sub.add_parser("show", help="resolve a revision and show its evidence")
    show.add_argument("sha")
    latest = sub.add_parser("latest", help="show the most recently updated record")
    check = sub.add_parser("check", help="validate schema and resolve every stored commit without recording evidence")
    for command in (record, show, latest, check):
        command.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    args = parser.parse_args(argv)
    try:
        if args.command == "check":
            for row in read_ledger(args.ledger)["records"]:
                if resolve_commit(row["sha"]) != row["sha"]:
                    raise ValueError("noncanonical commit in ledger")
            print("verified ledger valid")
            return 0
        if args.command == "record":
            updates = {field: getattr(args, field) for field in FIELDS
                       if field != "captures" and getattr(args, field) is not None}
            if args.capture:
                updates["captures"] = list(dict.fromkeys(args.capture))
            value = record_proof(args.ledger, args.sha, updates)
        elif args.command == "show":
            value = show_proof(args.ledger, args.sha)
        else:
            value = latest_proof(args.ledger)
        if value is None:
            print("no verified evidence for this query", file=sys.stderr)
            return 1
        print(json.dumps(value, indent=2))
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
