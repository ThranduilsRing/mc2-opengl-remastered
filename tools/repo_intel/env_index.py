#!/usr/bin/env python3
"""
REPO-INTEL-1b: env_index.py
Build a unified env var index by merging three sources:
  1. Gates/gate_table.json                — the compiled gate table (most authoritative)
  2. docs/tier1_env_vars.md               — bullet-prose user docs
  3. getenv("MC2_*") grep across source   — ground truth for all vars
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# The registry is read from the compiled gate table (Gates/gate_table.json, emitted by tools/gate_dump), never
# parsed from C++. A reader that cannot read it RAISES (GateTableError, kept under its old name
# RegistryParseError for callers): "I could not read the source" must never be reported as "this gate is
# not registered". See gate_status().
_SCRIPTS_DIR = str(Path(__file__).resolve().parents[2] / "scripts")
if _SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, _SCRIPTS_DIR)
try:
    from gate_table_reader import GateTableError as RegistryParseError, registry_entries as _registry_entries
except ImportError as _exc:                                   # pragma: no cover
    # Deliberately NOT a module-level hard failure: this module is imported by the MCP server, and taking the
    # whole server down would also take out mission_facts, preflight and lane_registry, none of which read the
    # registry. Scope the failure to the registry question instead, as a refusal.
    _IMPORT_ERROR = _exc

    class RegistryParseError(Exception):
        pass

    def _registry_entries(root):
        raise RegistryParseError(
            "the gate table reader (scripts/gate_table_reader.py) could not be imported (%s) -- refusing to "
            "report an empty gate registry" % _IMPORT_ERROR)


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_WALK_EXCLUDE_DIRS = {"build64", "3rdparty", ".git", "releases", "dist",
                      "release", ".vscode", ".clangd", ".ccls-cache",
                      # The gate table itself names every gate as a row; counting those as readers
                      # would make every gate look read.
                      "Gates"}
# The gate module's unit test names gates as synthetic fixtures, not as readers.
_WALK_EXCLUDE_FILES = {"tests/unit/test_gate.cpp"}
_WALK_EXTENSIONS   = {".cpp", ".h", ".hpp", ".c"}

# Match direct getenv("MC2_*"), std::getenv, and common thin wrappers:
#   envFlag*("MC2_*") (envFlagOn/envFlag/envFlagDefaultOn/...), selftestEnvFlag("MC2_*"),
#   getEnvVar("MC2_*").
_GETENV_VAR    = re.compile(
    r'(?:std::)?(?:getenv|envFlag\w*|selftestEnvFlag|getEnvVar)\s*\(\s*"(MC2_[A-Z0-9_]+)"'
)
# Fallback: a "MC2_*" literal as the sole/first argument to ANY call. Catches
# unknown per-TU helper wrappers that the explicit alternation above misses.
_GETENV_VAR_ANYCALL = re.compile(
    r'\w+\s*\(\s*"(MC2_[A-Z0-9_]+)"'
)
# Typed gate registry readers: gate::get<"MC2_X">(), get<"MC2_X", Rule::...>(), state/isSet/str/is<"MC2_X"...>.
_GATE_GET_VAR  = re.compile(
    r'\b(?:(?:mc2::)?gate::)?(?:get|state|isSet|str|is)\s*<\s*"(MC2_[A-Z0-9_]+)"'
)
_TIER1_VAR     = re.compile(r'`(MC2_[A-Z_0-9]+)(?:=[^`]*)?`')
_TIER1_DEFAULT = re.compile(r'Default\s+\*\*(ON|OFF)\*\*', re.IGNORECASE)


# ---------------------------------------------------------------------------
# Source parsers
# ---------------------------------------------------------------------------

def _parse_registry(repo_root: Path) -> dict:
    """Returns {envVarName: {feature_id, kind, default, doc}} from the compiled gate table (registry(...) rows).

    RAISES RegistryParseError when the header is missing, unreadable, or does
    not parse completely.  It used to return {} for all of those AND for "the
    registry really is empty", which made "I could not read the source" and
    "this gate is not registered" the same bytes to every caller.  They are
    opposite facts, and a caller that cannot tell them apart will publish the
    wrong one with a straight face.
    """
    reg = _registry_entries(repo_root)

    out = {}
    for entry in reg.values():
        if entry["envVar"] is None:
            continue  # always-on feature, no runtime gate
        out[entry["envVar"]] = {
            "feature_id": entry["featureId"],
            "kind":       entry["kind"],
            "default":    "on" if entry["defaultOn"] else "off",
            "doc":        entry["doc"],
            "source":     "Gates/gate_table.json",
        }
    return out


def _parse_tier1_docs(repo_root: Path) -> dict:
    """Returns {varName: {default, raw_line}} from docs/tier1_env_vars.md."""
    path = repo_root / "docs" / "tier1_env_vars.md"
    if not path.exists():
        return {}

    text = path.read_text(encoding="utf-8", errors="replace")
    out  = {}

    # Split on bullet boundaries: lines starting with "- "
    paragraphs = re.split(r'\n(?=- )', text)

    for para in paragraphs:
        var_m = _TIER1_VAR.search(para)
        if not var_m:
            continue
        name  = var_m.group(1)
        def_m = _TIER1_DEFAULT.search(para)
        # First non-empty line of the bullet as raw description
        first = next((ln.strip() for ln in para.splitlines() if ln.strip()), "")
        out[name] = {
            "default":  def_m.group(1).lower() if def_m else None,
            "raw_line": first[:200],
        }

    return out


def _grep_getenv(repo_root: Path) -> dict:
    """Walk C++ source for getenv("MC2_*"). Returns {name: [{"file":..., "line":N}]}.
    Uses Python file walking to avoid subprocess quoting issues on Windows."""
    readers: dict = {}

    for dirpath, dirnames, filenames in os.walk(repo_root):
        # Prune excluded dirs in-place
        dirnames[:] = [
            d for d in dirnames
            if d not in _WALK_EXCLUDE_DIRS and not d.startswith("build")
        ]
        for fname in filenames:
            if Path(fname).suffix not in _WALK_EXTENSIONS:
                continue
            fpath = Path(dirpath) / fname
            try:
                text = fpath.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if 'MC2_' not in text:
                continue
            rel = str(fpath.relative_to(repo_root)).replace("\\", "/")
            if rel in _WALK_EXCLUDE_FILES:
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                seen_on_line = set()
                for m in _GATE_GET_VAR.finditer(line):
                    name = m.group(1)
                    if name in seen_on_line:
                        continue
                    seen_on_line.add(name)
                    readers.setdefault(name, []).append({"file": rel, "line": lineno})
                for m in _GETENV_VAR.finditer(line):
                    name = m.group(1)
                    seen_on_line.add(name)
                    readers.setdefault(name, []).append({"file": rel, "line": lineno})
                # Fallback: treat a "MC2_*" literal passed as the first arg to ANY
                # call as a code reader too (covers custom per-TU wrappers). Skip
                # names already captured by the explicit pass on this line.
                for m in _GETENV_VAR_ANYCALL.finditer(line):
                    name = m.group(1)
                    if name in seen_on_line:
                        continue
                    seen_on_line.add(name)
                    readers.setdefault(name, []).append({"file": rel, "line": lineno})

    return readers


# ---------------------------------------------------------------------------
# Index builder
# ---------------------------------------------------------------------------

def build_index(repo_root: Path) -> dict:
    """Build unified env var index. Returns {MC2_NAME: entry_dict}."""
    registry = _parse_registry(repo_root)
    tier1    = _parse_tier1_docs(repo_root)
    readers  = _grep_getenv(repo_root)

    all_names = sorted(set(registry) | set(tier1) | set(readers))
    index: dict = {}

    for name in all_names:
        entry: dict = {
            "name":       name,
            "sources":    [],
            "readers":    readers.get(name, []),
            "documented": False,
        }

        if name in registry:
            r = registry[name]
            entry["sources"].append("registry")
            entry["feature_id"] = r["feature_id"]
            entry["kind"]       = r["kind"]
            entry["default"]    = r["default"]
            entry["doc"]        = r["doc"]
            entry["documented"] = True

        if name in tier1:
            t = tier1[name]
            entry["sources"].append("tier1_doc")
            entry["documented"] = True
            # tier1 default fills gap if registry didn't set one
            if "default" not in entry and t.get("default"):
                entry["default"] = t["default"]
            # tier1 doc fills gap if registry didn't provide doc
            if "doc" not in entry:
                entry["doc"] = t["raw_line"]

        if readers.get(name):
            if "getenv_grep" not in entry["sources"]:
                entry["sources"].append("getenv_grep")

        # Ensure default key always present
        entry.setdefault("default", None)

        index[name] = entry

    return index


# ---------------------------------------------------------------------------
# Query interface
# ---------------------------------------------------------------------------

def query_env(
    repo_root: Path,
    name: str         = None,
    domain: str       = None,
    undocumented: bool = False,
    show_all: bool    = False,
) -> dict:
    """
    Query the env var index.
    Modes (mutually exclusive in priority order):
      name=MC2_FOO   → look up specific var (fuzzy if no exact match)
      undocumented   → vars present in code but not in registry or tier1 docs
      domain=shadow  → filter by substring in name
      show_all       → dump everything
    """
    index = build_index(repo_root)

    # Summary always computed
    summary = {
        "total":        len(index),
        "documented":   sum(1 for v in index.values() if v["documented"]),
        "undocumented": sum(1 for v in index.values() if not v["documented"]),
        "in_registry":  sum(1 for v in index.values() if "registry"   in v["sources"]),
        "in_tier1_doc": sum(1 for v in index.values() if "tier1_doc"  in v["sources"]),
        "grep_only":    sum(1 for v in index.values()
                           if v["sources"] == ["getenv_grep"]),
    }

    if name:
        name_up = name.upper()
        if name_up in index:
            return {"query": name_up, "result": index[name_up]}
        # Substring fuzzy match
        matches = {k: v for k, v in index.items() if name_up in k}
        if matches:
            return {
                "query":   name_up,
                "matches": sorted(matches.keys()),
                "results": matches,
            }
        return {
            "query": name_up,
            "error": "not found",
            "total_known": len(index),
        }

    filtered = dict(index)

    if undocumented:
        filtered = {k: v for k, v in filtered.items() if not v["documented"]}

    if domain:
        dom = domain.upper()
        filtered = {k: v for k, v in filtered.items() if dom in k}

    if not show_all and not undocumented and not domain:
        # Default: return summary only (don't dump 282 entries)
        return {"summary": summary}

    return {
        "summary": summary,
        "count":   len(filtered),
        "vars":    filtered,
    }
