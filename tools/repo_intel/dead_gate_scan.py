#!/usr/bin/env python3
"""
REPO-INTEL: dead_gate_scan.py
Read-only ADVISORY classifier for MC2_* env gates by DELETABILITY EVIDENCE.

CRITICAL FRAMING — read before using any output:
  env-gated-OFF is NOT dead. Default-OFF is the project's deliberate feature-gate
  system. Default-OFF gates include LIVE FIXES (e.g. MC2_ANIM_CADENCE_FIX) and
  SHIPPED FEATURES (e.g. MC2_ASSIMP_MECH_IMPORT). So a gate's DEFAULT STATE is
  IRRELEVANT to deletability. This tool classifies by CODE-PATH EVIDENCE ONLY.

  The tool is ADVISORY. It never deletes anything. Deletion is ALWAYS
  human-gated: a human must verify each TIER_A entry before removing a gate.

Reuses the existing env index (env_index.query_env(root, show_all=True)) for the
gate universe (readers/documented/default/sources), then gathers per-gate
deletability evidence by reading bounded windows around reader sites.

Tiers:
  TIER_A_dead       — high-confidence orphaned/dead code path. REVIEW THEN DELETE.
  TIER_B_diag_strip — post-ship single-reader diagnostic trace. Strippable clutter.
  TIER_C_keep       — feature/fix/guard gates. DO NOT DELETE (informational).
"""

import os
import re
from pathlib import Path

import env_index as ei


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_SOURCE_EXTS = {".cpp", ".h", ".hpp", ".c", ".frag", ".vert", ".comp",
                ".geom", ".tesc", ".tese", ".hglsl", ".py"}

# Reader window for evidence gathering (lines on each side of a reader line).
_WINDOW = 6
# Wider window for the "result unused" lexical check.
_USE_WINDOW = 15

# Caps to keep the scan bounded.
_MAX_FILE_BYTES   = 2 * 1024 * 1024   # skip files > 2MB
_MAX_FILE_READS   = 4000              # total distinct file reads cap
_TIER_A_CAP       = 150
_TIER_B_CAP       = 200

# DELETE-INTENT markers that, in a doc/ledger, indicate a gate was actually
# removed/reverted. These DRIVE the ledger_removed SIGNAL (TIER_A).
_LEDGER_MARKERS = ("REMOVED", "DELETED", "REVERTED")
# History/status words that are advisory only — they describe a gate's lifecycle
# but do NOT by themselves imply deletion. Surfaced as `history_marker` but they
# must not classify a gate as TIER_A on their own.
_HISTORY_MARKERS = ("DEAD", "MOOT", "SUPERSEDED", "SHELVED")

_DIAG_NAME_RE = re.compile(r"_(TRACE|DIAG|DEBUG|PROBE)\b")

_GATE_NAME_RE = re.compile(r"MC2_[A-Z0-9_]+")

# getenv-style call we care about for result-unused analysis.
_GETENV_CALL_RE = re.compile(
    r'(?:std::)?(?:getenv|envFlag\w*|selftestEnvFlag|getEnvVar)\s*\(\s*"(MC2_[A-Z0-9_]+)"\s*\)'
    # typed gate registry: gate::get<"MC2_X">() / get<"MC2_X", Rule::...>() / state<>/isSet<>
    r'|\b(?:(?:mc2::)?gate::)?(?:get|state|isSet)\s*<\s*"(MC2_[A-Z0-9_]+)"\s*(?:,[^>]*)?>\s*\(\s*\)'
)


def _read_call(gate_name):
    """Regex source for one read of `gate_name`: a getenv-style call or a gate::get<> read."""
    g = re.escape(gate_name)
    return (r'(?:(?:std::)?(?:getenv|envFlag\w*|selftestEnvFlag|getEnvVar)\s*\(\s*"' + g + r'"'
            r'|\b(?:(?:mc2::)?gate::)?(?:get|state|isSet)\s*<\s*"' + g + r'"\s*(?:,[^>]*)?>\s*\(\s*)')

_NOTE = (
    "ADVISORY ONLY — default-OFF is NOT dead (it is the deliberate feature-gate "
    "system; default state is irrelevant to deletability). Classification is by "
    "code-path EVIDENCE only. Verify every TIER_A entry by hand before deleting "
    "any gate; deletion is always human-gated."
)


# ---------------------------------------------------------------------------
# File cache (bounded)
# ---------------------------------------------------------------------------

class _FileCache:
    def __init__(self, root: Path):
        self.root = root
        self.cache = {}          # rel_path -> list[str] lines (or None if skipped)
        self.reads = 0

    def lines(self, rel_path: str):
        if rel_path in self.cache:
            return self.cache[rel_path]
        if self.reads >= _MAX_FILE_READS:
            self.cache[rel_path] = None
            return None
        fpath = self.root / rel_path
        try:
            if fpath.stat().st_size > _MAX_FILE_BYTES:
                self.cache[rel_path] = None
                return None
            text = fpath.read_text(encoding="utf-8", errors="replace")
        except OSError:
            self.cache[rel_path] = None
            return None
        self.reads += 1
        lines = text.splitlines()
        self.cache[rel_path] = lines
        return lines


def _is_source(rel_path: str) -> bool:
    return Path(rel_path).suffix.lower() in _SOURCE_EXTS


# ---------------------------------------------------------------------------
# Per-file structural scans (cached per file)
# ---------------------------------------------------------------------------

def _if0_line_set(lines):
    """Return the set of 1-based line numbers that fall inside a #if 0 block.
    Best-effort: tracks #if/#ifdef/#ifndef nesting; a region is 'dead' while any
    enclosing #if 0 is active."""
    dead = set()
    stack = []  # each entry: True if this level is a dead (#if 0) branch
    for i, raw in enumerate(lines, 1):
        s = raw.strip()
        if re.match(r'^#\s*if\b', s):
            is_if0 = bool(re.match(r'^#\s*if\s+0\b', s))
            stack.append(is_if0)
        elif re.match(r'^#\s*(ifdef|ifndef)\b', s):
            stack.append(False)
        elif re.match(r'^#\s*elif\b', s):
            if stack:
                # leaving any if-0 branch on elif; treat elif branch as live
                stack[-1] = False
        elif re.match(r'^#\s*else\b', s):
            if stack:
                # the #else of an #if 0 is the LIVE branch
                stack[-1] = False
        elif re.match(r'^#\s*endif\b', s):
            if stack:
                stack.pop()
            continue
        if any(stack):
            dead.add(i)
    return dead


# ---------------------------------------------------------------------------
# Ledger (md) scan — name -> marker
# ---------------------------------------------------------------------------

def _build_ledger_map(root: Path) -> tuple:
    """Scan .claude/*.md and docs/**/*.md once.

    Returns (delete_map, history_map):
      delete_map  — {gate_name: marker} when a gate name appears on the SAME line
                    as a DELETE-INTENT marker (REMOVED/DELETED/REVERTED). Drives
                    the ledger_removed SIGNAL. A real same-line hit may override a
                    stray earlier hit (no first-match-lock).
      history_map — {gate_name: marker} for advisory-only history words
                    (DEAD/MOOT/SUPERSEDED/SHELVED), same-line. Surfaced but does
                    NOT classify a gate as TIER_A.
    """
    delete_map = {}
    history_map = {}
    md_files = []
    for sub in (".claude", "docs"):
        base = root / sub
        if not base.is_dir():
            continue
        for dp, dns, fns in os.walk(base):
            dns[:] = [d for d in dns if d not in (".git",)]
            for fn in fns:
                if fn.lower().endswith(".md"):
                    md_files.append(Path(dp) / fn)

    delete_re  = re.compile(r'\b(' + "|".join(_LEDGER_MARKERS) + r')\b')
    history_re = re.compile(r'\b(' + "|".join(_HISTORY_MARKERS) + r')\b')
    for f in md_files:
        try:
            if f.stat().st_size > _MAX_FILE_BYTES:
                continue
            lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for ln in lines:
            del_m  = delete_re.search(ln)
            hist_m = history_re.search(ln) if not del_m else None
            if not del_m and not hist_m:
                continue
            for name in set(_GATE_NAME_RE.findall(ln)):
                # Same-line delete-intent overrides any prior proximity hit.
                if del_m:
                    delete_map[name] = del_m.group(1)
                elif hist_m and name not in history_map:
                    history_map[name] = hist_m.group(1)
    return delete_map, history_map


# ---------------------------------------------------------------------------
# Per-gate evidence
# ---------------------------------------------------------------------------

# Cached-bool / module-global idiom: a getenv result captured into a static or
# file-scope variable. The consumer of such a var lives elsewhere (often another
# TU), so the result is USED-by-default — never flag these as result_unused.
_CACHED_BOOL_RE = re.compile(
    r'^\s*(?:static\s+)?(?:const\s+)?'
    r'(?:bool|char\s*\*|const\s+char\s*\*|auto)\s+\w+\s*=\s*'
    r'.*(?:getenv|envFlag\w*|selftestEnvFlag|gate::get\s*<)')


def _enclosing_function_end(lines, idx) -> int:
    """Best-effort: from reader line `idx`, walk forward by brace depth to the end
    of the enclosing function. Returns an exclusive end index. Bounded by a sane
    cap so a malformed/global context never scans unboundedly."""
    cap = min(len(lines), idx + 400)
    depth = 0
    seen_open = False
    for k in range(idx, cap):
        depth += lines[k].count("{") - lines[k].count("}")
        if lines[k].count("{"):
            seen_open = True
        if seen_open and depth <= 0:
            return k + 1
    return cap


def _result_unused_at(lines, line_no, gate_name) -> bool:
    """Best-effort lexical: is this getenv(gate) result unused?
    True when the getenv call appears as a bare statement (no '=' assignment and
    not inside an if/while/return/argument context) OR is assigned to a var that
    never appears again in the use-search window."""
    idx = line_no - 1
    if idx < 0 or idx >= len(lines):
        return False
    line = lines[idx]
    if not _GETENV_CALL_RE.search(line):
        # The reader may be a wrapper not matching; cannot judge — say used.
        return False

    # Cached-bool / module-global idiom is USED-by-default (consumer elsewhere).
    if _CACHED_BOOL_RE.search(line):
        return False

    # Bare call: line is essentially just `getenv("X");` with nothing consuming it.
    stripped = line.strip()
    bare = bool(re.match(r'^' + _read_call(gate_name) + r'\)\s*;?\s*$', stripped))
    if bare:
        return True

    # Assignment form: `<type> var = getenv("X");` — capture LHS var name.
    m = re.search(r'([A-Za-z_]\w*)\s*=\s*' + _read_call(gate_name), line)
    if m:
        var = m.group(1)
        var_re = re.compile(r'\b' + re.escape(var) + r'\b')
        # Same-line consumption (e.g. `const char* e = getenv(X); return e && ...`,
        # common in one-line lambdas) counts as USED — search the reader line tail
        # after the assignment match.
        if var_re.search(line[m.end():]):
            return False
        # Detect file-scope/global LHS (column-0 assignment, no leading indent):
        # scan the whole file. Otherwise scan to the end of the enclosing function.
        is_global = (line[:1] not in (" ", "\t"))
        if is_global:
            lo, hi = 0, len(lines)
        else:
            lo = max(0, idx - _USE_WINDOW)
            hi = _enclosing_function_end(lines, idx)
        uses = 0
        for k in range(lo, hi):
            if k == idx:
                continue
            if var_re.search(lines[k]):
                uses += 1
        if uses == 0:
            return True
    return False


def _classify_gate(entry, fc: _FileCache, if0_cache: dict,
                   ledger_map: dict, history_map: dict):
    """Return (tier, evidence_dict) for one gate entry."""
    name = entry["name"]
    readers = entry.get("readers", []) or []
    reader_files = sorted({r["file"] for r in readers})
    reader_count = len(readers)

    code_readers = [r for r in readers if _is_source(r["file"])]
    has_code_reader = bool(code_readers)
    # doc_only: gate has NO code reader (env_index only walks source, so any reader
    # in 'readers' is already a code reader; doc_only means the index found the gate
    # via registry/tier1 docs but no getenv reader at all).
    sources = entry.get("sources", []) or []
    doc_only = (not has_code_reader)

    in_if0 = False
    result_unused = False
    header_reader = False
    cross_tu_global = False
    for r in code_readers:
        rel = r["file"]
        if Path(rel).suffix.lower() in {".h", ".hpp", ".hglsl"}:
            header_reader = True
        lines = fc.lines(rel)
        if lines is None:
            continue
        # Only C-family files can have #if 0.
        if Path(rel).suffix.lower() in {".cpp", ".h", ".hpp", ".c",
                                        ".frag", ".vert", ".comp", ".geom",
                                        ".tesc", ".tese", ".hglsl"}:
            key = rel
            if key not in if0_cache:
                if0_cache[key] = _if0_line_set(lines)
            if r["line"] in if0_cache[key]:
                in_if0 = True
        if _result_unused_at(lines, r["line"], name):
            result_unused = True
        # cross_tu_global (best-effort): getenv feeds a non-static file-scope
        # symbol — assignment at column 0 with no leading `static`.
        ridx = r["line"] - 1
        if 0 <= ridx < len(lines):
            rline = lines[ridx]
            if (rline[:1] not in (" ", "\t")
                    and not rline.lstrip().startswith("static")
                    and re.search(r'=\s*(?:(?:std::)?(?:getenv|envFlag\w*|'
                                  r'selftestEnvFlag|getEnvVar)\s*\(|(?:mc2::)?gate::get\s*<)', rline)):
                cross_tu_global = True

    ledger_marker  = ledger_map.get(name)
    ledger_removed = ledger_marker is not None
    history_marker = history_map.get(name)

    name_class = "DIAG" if _DIAG_NAME_RE.search(name) else "FEATURE"

    in_registry = "registry" in sources
    in_tier1    = "tier1_doc" in sources

    # ---- Tier decision ----
    reasons = []
    if in_if0:
        reasons.append("in_if0")
    if result_unused:
        reasons.append("result_unused")
    if ledger_removed and has_code_reader:
        reasons.append("ledger_removed_with_live_reader")
    if doc_only:
        reasons.append("doc_only_no_code_reader")

    if reasons:
        tier = "A"
    elif (name_class == "DIAG" and not in_registry and not in_tier1
          and len(reader_files) == 1):
        tier = "B"
    else:
        tier = "C"

    # ---- Confidence sub-tier (TIER_A only) ----
    confidence = None
    if tier == "A":
        if reasons == ["in_if0"]:
            confidence = "A1"
        elif "result_unused" in reasons:
            confidence = "A2"
        else:  # ledger_removed / doc_only driven
            confidence = "A3"

    risk_flags = []
    if header_reader:
        risk_flags.append("header_reader")
    if cross_tu_global:
        risk_flags.append("cross_tu_global")

    evidence = {
        "name": name,
        "reader_count": reader_count,
        "reader_files": reader_files,
        "code_reader": has_code_reader,
        "doc_only": doc_only,
        "in_if0": in_if0,
        "result_unused": result_unused,
        "ledger_removed": ledger_removed,
        "ledger_marker": ledger_marker,
        "history_marker": history_marker,
        "name_class": name_class,
        "in_registry": in_registry,
        "in_tier1_doc": in_tier1,
        "reason": reasons,
        "confidence": confidence,
        "risk_flags": risk_flags,
        "readers": readers[:6],
    }
    return tier, evidence


# ---------------------------------------------------------------------------
# Public entry
# ---------------------------------------------------------------------------

def dead_gate_scan(repo_root, tier: str = "all", name: str = "") -> dict:
    """Classify every MC2_* gate by deletability evidence. Read-only, advisory.

    Args:
      repo_root — worktree root.
      tier      — "A" | "B" | "all" (filters which tiers populate the lists).
      name      — optional substring filter on gate name.

    Returns the JSON-able result dict (see module docstring for framing)."""
    root = Path(repo_root)
    tier = (tier or "all").upper()
    name_filter = (name or "").upper()

    env = ei.query_env(root, show_all=True)
    gates = env.get("vars", {})

    fc = _FileCache(root)
    if0_cache = {}
    ledger_map, history_map = _build_ledger_map(root)

    tier_a, tier_b = [], []
    tier_c_count = 0
    totals_tier = {"A": 0, "B": 0, "C": 0}

    for gname in sorted(gates):
        if name_filter and name_filter not in gname:
            continue
        entry = gates[gname]
        t, ev = _classify_gate(entry, fc, if0_cache, ledger_map, history_map)
        totals_tier[t] += 1

        if t == "A":
            tier_a.append({
                "name": ev["name"],
                "reason": ev["reason"],
                "confidence": ev["confidence"],
                "risk_flags": ev["risk_flags"],
                "readers": ev["readers"],
                "ledger_marker": ev["ledger_marker"],
                "history_marker": ev["history_marker"],
            })
        elif t == "B":
            tier_b.append({
                "name": ev["name"],
                "reader_file": ev["reader_files"][0] if ev["reader_files"] else None,
            })
        else:
            tier_c_count += 1

    # Apply caps.
    tier_a_truncated = len(tier_a) > _TIER_A_CAP
    tier_a_out = tier_a[:_TIER_A_CAP]
    tier_b_out = tier_b[:_TIER_B_CAP]

    # Tier filtering of the emitted lists (totals always reflect full classification).
    if tier == "A":
        tier_b_out = []
    elif tier == "B":
        tier_a_out = []

    result = {
        "confidence": "heuristic",
        "note": _NOTE,
        "totals": {
            "gates":  totals_tier["A"] + totals_tier["B"] + totals_tier["C"],
            "tier_a": totals_tier["A"],
            "tier_b": totals_tier["B"],
            "tier_c": totals_tier["C"],
        },
        "tier_a": tier_a_out,
        "tier_b": tier_b_out,
        "tier_c_count": tier_c_count,
    }
    if tier_a_truncated:
        result["tier_a_truncated"] = True
    if name_filter:
        result["name_filter"] = name_filter
    if tier != "ALL":
        result["tier_filter"] = tier
    return result
