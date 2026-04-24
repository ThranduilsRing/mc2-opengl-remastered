# scripts/smoke_lib/baselines.py
"""Baseline load/store keyed by <profile>@<stem>@<tier>@<duration>."""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


def key(profile: str, stem: str, tier: str, duration: int) -> str:
    return f"{profile}@{stem}@{tier}@{duration}"


def load(path: Path) -> dict:
    if not path.exists(): return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")


def destroys_delta(baselines: dict, key_str: str, observed: int) -> Optional[int]:
    b = baselines.get(key_str, {}).get("destroys", {})
    mean = b.get("mean")
    if mean is None: return None
    return observed - int(mean)
