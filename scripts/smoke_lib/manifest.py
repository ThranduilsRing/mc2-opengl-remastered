# scripts/smoke_lib/manifest.py
"""MC2 smoke mission manifest parser.

Line grammar (whitespace-split with shell-style quoting):
    <tier> <stem> [key=value]... [reason="..."]

Tiers: tier1 | tier2 | tier3 | skip
Unknown keys emit a warning (stderr) but are not errors.
"""
from __future__ import annotations

import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

VALID_TIERS = {"tier1", "tier2", "tier3", "skip"}
VALID_KEYS = {"duration", "heartbeat_timeout_load", "heartbeat_timeout_play",
              "profile", "active", "reason"}


@dataclass
class Entry:
    tier: str
    stem: str
    duration: Optional[int] = None
    heartbeat_timeout_load: Optional[int] = None
    heartbeat_timeout_play: Optional[int] = None
    profile: Optional[str] = None
    active: bool = False
    reason: str = ""
    source_line: int = 0


def parse_manifest(path: Path) -> list[Entry]:
    text = Path(path).read_text()
    out: list[Entry] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = shlex.split(line, posix=True)
        if len(tokens) < 2:
            print(f"[manifest] warn: line {lineno} too short: {raw!r}", file=sys.stderr)
            continue
        tier, stem = tokens[0], tokens[1]
        if tier not in VALID_TIERS:
            print(f"[manifest] warn: line {lineno} unknown tier {tier!r}",
                  file=sys.stderr)
            continue
        e = Entry(tier=tier, stem=stem, source_line=lineno)
        for kv in tokens[2:]:
            if "=" not in kv:
                print(f"[manifest] warn: line {lineno} bad token {kv!r}",
                      file=sys.stderr)
                continue
            k, v = kv.split("=", 1)
            if k not in VALID_KEYS:
                print(f"[manifest] warn: line {lineno} unknown key {k!r}",
                      file=sys.stderr)
                continue
            if k == "duration":
                e.duration = int(v)
            elif k == "heartbeat_timeout_load":
                e.heartbeat_timeout_load = int(v)
            elif k == "heartbeat_timeout_play":
                e.heartbeat_timeout_play = int(v)
            elif k == "profile":
                e.profile = v
            elif k == "active":
                e.active = v.lower() in ("1", "true", "yes")
            elif k == "reason":
                e.reason = v
        out.append(e)
    return out
