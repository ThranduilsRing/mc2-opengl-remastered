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

from .logparse import MARKER_BUCKETS

VALID_TIERS = {"tier1", "tier2", "tier3", "skip"}
VALID_KEYS = {"duration", "heartbeat_timeout_load", "heartbeat_timeout_play",
              "profile", "active", "reason", "allow_asset_oob", "mod", "deps"}

# META-FIX INFRA-3: manifest override keys registered by marker-bucket rows
# (allow_asset_oob analog — downgrades that bucket to advisory per mission).
MARKER_OVERRIDE_KEY_TO_BUCKET = {
    mb.manifest_override_key: mb.bucket
    for mb in MARKER_BUCKETS if mb.manifest_override_key
}
VALID_KEYS |= set(MARKER_OVERRIDE_KEY_TO_BUCKET)


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
    allow_asset_oob: bool = False
    # SMOKE-MODMISSION-ENV-GUARDS-1: structured mod fields.
    # mod: exact mods/<DirName> for the mod this mission belongs to (empty = stock).
    # deps: comma-separated compat layer dir names (e.g. "mc2x-compat,mco-compat").
    mod: str = ""
    deps: str = ""
    # META-FIX INFRA-3: per-mission marker-bucket downgrades (bucket -> True).
    marker_overrides: dict = field(default_factory=dict)
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
            elif k == "allow_asset_oob":
                e.allow_asset_oob = v.lower() in ("1", "true", "yes")
            elif k == "mod":
                e.mod = v
            elif k == "deps":
                e.deps = v
            elif k in MARKER_OVERRIDE_KEY_TO_BUCKET:
                if v.lower() in ("1", "true", "yes"):
                    e.marker_overrides[MARKER_OVERRIDE_KEY_TO_BUCKET[k]] = True
        out.append(e)
    return out
