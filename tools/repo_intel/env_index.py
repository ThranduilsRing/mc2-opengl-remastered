#!/usr/bin/env python3
"""
REPO-INTEL-1b: env_index.py
Build a unified env var index by merging three sources:
  1. RenderCore/RendererFeatureRegistry.h  — structured C++ (most authoritative)
  2. docs/tier1_env_vars.md               — bullet-prose user docs
  3. getenv("MC2_*") grep across source   — ground truth for all vars
"""

import os
import re
import subprocess
from pathlib import Path


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_WALK_EXCLUDE_DIRS = {"build64", "3rdparty", ".git", "releases", "dist",
                      "release", ".vscode", ".clangd", ".ccls-cache"}
_WALK_EXTENSIONS   = {".cpp", ".h", ".hpp", ".c"}

_REGISTRY_ENTRY = re.compile(
    r'\{\s*'
    r'"(MC2_[A-Z0-9_]+)",\s*'            # featureId — kFeatureTable MC2_FEATURE_*
                                         #  AND kAuxEnvVars MC2_DIAG_*/MC2_TUNE_*
                                         #  (MCP-ANTI-CHURN-1: aux entries were
                                         #  missed -> false "unregistered" verdicts)
    r'(nullptr|"MC2_[A-Z_0-9]+")\s*,\s*' # envVar (may be nullptr)
    r'EnvVarKind::(\w+)\s*,\s*'           # kind
    r'(true|false)\s*,\s*'                # defaultOn
    r'"((?:[^"\\]|\\.)*)"',               # doc (allow escaped chars)
    re.MULTILINE | re.DOTALL,
)

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
_TIER1_VAR     = re.compile(r'`(MC2_[A-Z_0-9]+)(?:=[^`]*)?`')
_TIER1_DEFAULT = re.compile(r'Default\s+\*\*(ON|OFF)\*\*', re.IGNORECASE)


# ---------------------------------------------------------------------------
# Source parsers
# ---------------------------------------------------------------------------

def _parse_registry(repo_root: Path) -> dict:
    """Returns {envVarName: {feature_id, kind, default, doc}} from RendererFeatureRegistry.h."""
    path = repo_root / "RenderCore" / "RendererFeatureRegistry.h"
    if not path.exists():
        return {}

    text = path.read_text(encoding="utf-8", errors="replace")
    out  = {}

    for m in _REGISTRY_ENTRY.finditer(text):
        feature_id  = m.group(1)
        env_raw     = m.group(2)
        kind        = m.group(3)
        default_on  = (m.group(4) == "true")
        doc         = m.group(5)

        if env_raw == "nullptr":
            continue  # always-on feature, no runtime gate

        name = env_raw.strip('"')
        out[name] = {
            "feature_id": feature_id,
            "kind":       kind,
            "default":    "on" if default_on else "off",
            "doc":        doc,
            "source":     "RenderCore/RendererFeatureRegistry.h",
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
            for lineno, line in enumerate(text.splitlines(), 1):
                seen_on_line = set()
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
