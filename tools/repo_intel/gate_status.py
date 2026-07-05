#!/usr/bin/env python3
"""
MCP-ANTI-CHURN-1: gate_status.py
One-verdict MC2_* gate audit: merges FOUR sources that today live in four
different places and drifted independently (2 observed ENV-DROP incidents +
11 unregistered vars on 2026-07-01):

  1. RenderCore/RendererFeatureRegistry.h        (env_index registry parse)
  2. docs/tier1_env_vars.md                      (env_index tier1 parse)
  3. scripts/run_smoke.py passthrough allowlist  (anchors `if k in (` .. `)},`
                                                  — same extraction as
                                                  scripts/check-env-allowlist.sh)
  4. scripts/check-env-registry.sh ALLOWLIST=( ) (legacy/trace var allowlist)

plus a live `git grep` for code readers (fast single-var path; env_index's
full source walk is only used as fallback).

Verdict flags map 1:1 to the failure classes:
  ENV_DROP_RISK   — read by engine but NOT in run_smoke passthrough allowlist
                    -> gate-ON smoke silently runs gate-OFF (Mistake E)
  UNREGISTERED    — not in RendererFeatureRegistry.h AND not in the
                    check-env-registry.sh ALLOWLIST -> CI gate fails
  UNDOCUMENTED    — no docs/tier1_env_vars.md entry
  GHOST           — registered/documented but no code reader found
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path
from typing import Optional

import env_index as ei

_MC2_NAME = re.compile(r"^MC2_[A-Z0-9_]+$")
_QUOTED_MC2 = re.compile(r'"(MC2_[A-Z0-9_]+)"')
_BARE_MC2 = re.compile(r"^\s*(MC2_[A-Z0-9_]+)\b")

_READER_DIRS = ("mclib", "GameOS", "code", "RenderCore", "RenderWorld",
                "GameAdapters", "editor", "gui")


# ---------------------------------------------------------------------------
# Source 3: run_smoke.py passthrough allowlist
# ---------------------------------------------------------------------------

def parse_smoke_allowlist(repo_root: Path) -> tuple[set, bool]:
    """Quoted MC2_* names inside every `if k in (` .. `)},` block of
    scripts/run_smoke.py. Returns (names, anchors_found)."""
    path = Path(repo_root) / "scripts" / "run_smoke.py"
    names: set = set()
    found = False
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return names, False
    in_block = False
    for line in text.splitlines():
        if not in_block and "if k in (" in line:
            in_block = True
            found = True
        if in_block:
            names.update(_QUOTED_MC2.findall(line))
            if ")}," in line:
                in_block = False
    return names, found


# ---------------------------------------------------------------------------
# Source 4: check-env-registry.sh ALLOWLIST
# ---------------------------------------------------------------------------

def parse_registry_check_allowlist(repo_root: Path) -> tuple[set, bool]:
    """MC2_* names inside the ALLOWLIST=( .. ) block of check-env-registry.sh."""
    path = Path(repo_root) / "scripts" / "check-env-registry.sh"
    names: set = set()
    found = False
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return names, False
    in_block = False
    for line in text.splitlines():
        if not in_block and line.strip().startswith("ALLOWLIST=("):
            in_block = True
            found = True
            continue
        if in_block:
            if line.strip() == ")":
                break
            m = _BARE_MC2.match(line)
            if m:
                names.add(m.group(1))
    return names, found


# ---------------------------------------------------------------------------
# Code readers (fast single-var git grep)
# ---------------------------------------------------------------------------

def _grep_readers(repo_root: Path, var: str, cap: int = 20) -> list[dict]:
    try:
        r = subprocess.run(
            ["git", "-C", str(repo_root), "grep", "-n", "--fixed-strings",
             f'"{var}"', "--", *_READER_DIRS],
            capture_output=True, text=True, timeout=15,
            stdin=subprocess.DEVNULL,
        )
    except Exception:
        return []
    readers = []
    for line in r.stdout.splitlines():
        parts = line.split(":", 2)
        if len(parts) == 3:
            readers.append({"file": parts[0].replace("\\", "/"),
                            "line": int(parts[1]) if parts[1].isdigit() else 0,
                            "snippet": parts[2].strip()[:160]})
        if len(readers) >= cap:
            break
    return readers


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def gate_status(repo_root: Path, var: str) -> dict:
    """Merged registered/allowlisted/documented/default verdict for one gate."""
    var = (var or "").strip()
    if not _MC2_NAME.match(var):
        return {"error": f"invalid gate name {var!r} (expected MC2_[A-Z0-9_]+)"}
    repo_root = Path(repo_root)

    registry = ei._parse_registry(repo_root)
    tier1 = ei._parse_tier1_docs(repo_root)
    smoke_allow, smoke_anchor_ok = parse_smoke_allowlist(repo_root)
    check_allow, check_anchor_ok = parse_registry_check_allowlist(repo_root)
    readers = _grep_readers(repo_root, var)

    reg = registry.get(var)
    t1 = tier1.get(var)
    in_registry = reg is not None
    in_tier1 = t1 is not None
    in_smoke_allow = var in smoke_allow
    in_check_allow = var in check_allow
    has_readers = bool(readers)

    default = None
    if reg and reg.get("default"):
        default = str(reg["default"]).upper()
    elif t1 and t1.get("default"):
        default = str(t1["default"]).upper()

    flags: list[str] = []
    if has_readers and not in_smoke_allow:
        flags.append("ENV_DROP_RISK: read by code but NOT in the run_smoke.py "
                     "passthrough allowlist — a gate-ON smoke may silently run "
                     "the gate-OFF path")
    if has_readers and not in_registry and not in_check_allow:
        flags.append("UNREGISTERED: not in RendererFeatureRegistry.h and not in "
                     "check-env-registry.sh ALLOWLIST — CI env-registry gate fails")
    if has_readers and not in_tier1:
        flags.append("UNDOCUMENTED: no docs/tier1_env_vars.md entry")
    if not has_readers and (in_registry or in_tier1 or in_smoke_allow or in_check_allow):
        flags.append("GHOST: registered/documented/allowlisted but no code reader "
                     "found (removed feature? doc litter?)")
    if not has_readers and not (in_registry or in_tier1 or in_smoke_allow or in_check_allow):
        flags.append("UNKNOWN: no trace of this var anywhere — typo?")

    parse_notes = []
    if not smoke_anchor_ok:
        parse_notes.append("run_smoke.py allowlist anchors not found — "
                           "smoke_allowlist result unreliable")
    if not check_anchor_ok:
        parse_notes.append("check-env-registry.sh ALLOWLIST block not found — "
                           "registry_check_allowlist result unreliable")

    return {
        "var": var,
        "verdict": "OK" if not flags else "GAPS",
        "flags": flags,
        "default": default,
        "registered": in_registry,
        "registry_entry": ({"feature_id": reg.get("feature_id"),
                            "kind": reg.get("kind"),
                            "default": reg.get("default"),
                            "doc": (reg.get("doc") or "")[:200]} if reg else None),
        "tier1_documented": in_tier1,
        "tier1_line": (t1.get("raw_line", "")[:200] if t1 else None),
        "smoke_allowlisted": in_smoke_allow,
        "registry_check_allowlisted": in_check_allow,
        "reader_count": len(readers),
        "readers": readers,
        "parse_notes": parse_notes,
        "confidence": "lexical",
    }
