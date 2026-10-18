"""
MC2 Mod Virtual Filesystem -- Static Resolver (S4)
====================================================
Pure-Python replica of the file.cpp resolution ladder for offline tooling.

THIS IS THE SINGLE SHARED MODULE for all VFS resolution consumers:
  - mc2mod check (mod-packaging-deploy-architecture.md §12)
  - build_index.py (data-ownership-registry-strategy.md §10)
  - parity_smoke.py (this slice, S4)

Import and call resolve() or build_mod_index() -- never reimplement the
resolution logic in another tool.  The engine (mclib/file.cpp) is the oracle;
this module must mirror it exactly.  The parity smoke (parity_smoke.py) is the
CI gate that enforces parity.

Spec refs: docs/superpowers/strategy/mod-virtual-filesystem-design.md §2, §4,
           §15.2; superpowers-execution-roadmap.md §5 S4 + ruling C5.

Ground truth: mclib/file.cpp in the worktree (line numbers verified 2026-06-11):
  - NormalizeKey      file.cpp:105
  - ShouldSearchMods  file.cpp:97
  - InitModSearchPaths file.cpp:486 (active mod -> dep[0]..dep[N-1])
  - TryModOpen        file.cpp:412
  - File::open READ   file.cpp:764-860
  - ReadModJson       file.cpp:393 (minimal JSON extractor)

Resolution order (from highest to lowest priority):
  1. g_modIndex first-wins (active mod, then dep[0]..dep[N-1])  -> layer "mod:<id>"
  2. base loose file (CWD-relative open)                        -> layer "base-loose"
  3. numeric-size-subdir strip (data/tgl/128/foo -> data/tgl/foo) -> layer "base-strip"
  4. FastFile (.fst membership, parsed from *.fst listing files) -> layer "fastfile"
  5. CD path (checkCDForFiles; unreachable in modern deploys)    -> layer "cd"
  6. not found                                                   -> layer "MISS"

JSONL record schema (matches engine MC2_RESOLVE_TRACE_FILE output, v=1):
  {
    "v": 1,
    "key":      "<normalized key>",
    "req":      "<original request path>",
    "layer":    "<layer name>",
    "path":     "<absolute path or empty>",
    "shadowed": ["<layer>", ...]   # layers that had the key but lost; [] if none
  }
  ("t" timestamp is engine-only; omitted by static resolver)

Config dict keys (passed to resolve() / load_config()):
  game_dir   str   Deploy root (CWD the engine runs from).  Required.
  mods_root  str   Path to mods/ folder.  Default: game_dir/mods.
  active_mod str   MC2_ACTIVE_MOD value.  None -> base-game mode (no mod layer).
  fst_files  list  Paths to .fst listing files.  Default: auto-discover game_dir/*.fst.

Python 3 stdlib only.  No engine edits, no run_smoke/gates.py edits.
"""

from __future__ import annotations

import os
import json
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Normalization -- mirrors file.cpp:105 NormalizeKey and file.cpp:743-752
# ---------------------------------------------------------------------------

def normalize_key(path: str) -> str:
    """
    Canonical key form: backslash -> forward-slash, ASCII lowercase.
    Mirrors NormalizeKey (file.cpp:105) + S_strlwr (file.cpp:744).

    Note: the engine lowercases the stored fileName before calling NormalizeKey,
    so even doNotLower callers end up with a lowercased key in TryModOpen.
    The static resolver therefore always lowercases -- the key lookup is safe
    regardless of the original casing of the request.
    """
    return path.replace("\\", "/").lower()


def should_search_mods(normalized_key: str) -> bool:
    """
    Mirrors ShouldSearchMods (file.cpp:97-103).
    Returns True iff the key is eligible for mod-index lookup:
      - non-empty index (handled by caller)
      - not an absolute path (no drive letter / leading slash)
      - no '..' component
      - starts with 'data/' or 'data\' (after normalization always 'data/')
    """
    if not normalized_key:
        return False
    # Absolute path: starts with drive letter (X:) or leading /
    if len(normalized_key) >= 2 and normalized_key[1] == ":":
        return False
    if normalized_key.startswith("/"):
        return False
    # Parent traversal
    if ".." in normalized_key.split("/"):
        return False
    # Must be under data/
    return normalized_key.startswith("data/")


# ---------------------------------------------------------------------------
# mod.json minimal extractor -- mirrors ReadModJson (file.cpp:393-409)
# ---------------------------------------------------------------------------

def _read_mod_json(json_path: str) -> Tuple[str, str, List[str]]:
    """
    Extract (id, name, dependencies[]) from a mod.json file.
    Uses only stdlib json; mirrors the minimal extractor in file.cpp:350-409.
    Returns ('', '', []) on parse failure (not found / malformed).
    """
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        mod_id = str(data.get("id", ""))
        mod_name = str(data.get("name", ""))
        deps_raw = data.get("dependencies", [])
        if isinstance(deps_raw, list):
            deps = [str(d) for d in deps_raw if isinstance(d, str)]
        else:
            deps = []
        return mod_id, mod_name, deps
    except (OSError, json.JSONDecodeError, ValueError):
        return "", "", []


# ---------------------------------------------------------------------------
# Index construction -- mirrors InitModSearchPaths (file.cpp:486-565)
# ---------------------------------------------------------------------------

def _index_mod_layer(
    index: Dict[str, Tuple[str, str]],        # key -> (abspath, modId)
    shadowed: Dict[str, List[str]],            # key -> [losing modIds]
    mod_data_dir: str,
    mod_id: str,
) -> None:
    """
    Walk mod_data_dir recursively and insert entries into index (first-wins).
    Mirrors IndexModData (file.cpp:208-272).

    Unified dot-dir/dot-file skip rule (ruling C4 in
    docs/superpowers/strategy/superpowers-execution-roadmap.md):
    Any entry whose name starts with '.' is skipped from the index.  This
    replaces four ad-hoc carve-outs (.scratch/, .modproject/, .playtest/,
    .modindex-cache) -- none of those dirs may ship their own per-dir skip
    logic.  Applies to both directories and files, matching the engine's
    fd.cFileName[0] == '.' guard at file.cpp:225 which covers both.
    No deployed mod (mods/mc2x-compat) relies on a dot-file being indexed.
    """
    data_dir = Path(mod_data_dir)
    if not data_dir.is_dir():
        return

    for root, dirs, files in os.walk(str(data_dir)):
        # Skip dot-prefixed directories (mirrors file.cpp:225)
        dirs[:] = [d for d in dirs if not d.startswith(".")]

        for fname in files:
            # Skip dot-prefixed files (mirrors file.cpp:225)
            if fname.startswith("."):
                continue
            abs_path = os.path.join(root, fname)
            # Compute relative path from the parent of data_dir so it starts "data/..."
            # data_dir itself is mods/<id>/data/; its parent is mods/<id>/
            try:
                rel = os.path.relpath(abs_path, str(data_dir.parent))
            except ValueError:
                # Different drives on Windows -- skip
                continue
            rel_norm = normalize_key(rel)
            if rel_norm in index:
                # First-wins: record the loser for shadowed-by info
                shadowed.setdefault(rel_norm, []).append(mod_id)
            else:
                index[rel_norm] = (abs_path.replace("\\", "/"), mod_id)


# ---------------------------------------------------------------------------
# FastFile (.fst) membership -- mirrors FastFileFind (fastfile.cpp:74-110)
# ---------------------------------------------------------------------------

def _build_fst_set(fst_listing_paths: List[str]) -> "set[str]":
    """
    Parse .fst listing files and return the set of normalized member keys.

    .fst listing format (one entry per line, path relative to deploy root):
      data/art/cursors1a.tga
    Lines starting with '#' are comments.

    The engine uses elfHash(filename) for O(1) lookup; the static resolver
    uses a set for simplicity (correctness-equivalent).

    If no listing files are provided, or none exist, returns an empty set
    (all base-game lookups fall through to MISS for fst-resident files).
    """
    members: set[str] = set()
    for path in fst_listing_paths:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    members.add(normalize_key(line))
        except OSError:
            pass
    return members


def _discover_fst_listings(game_dir: str) -> List[str]:
    """
    Auto-discover .fst listing files under game_dir.
    Looks for <game_dir>/*.fst.txt or <game_dir>/*.fst.listing patterns;
    for V1 these are the side-car text listings alongside .fst archives.
    Also accepts plain .fst files if they are text (some deploys ship them
    as newline-delimited listings; binary .fst archives are detected and
    skipped by checking for a null byte in the first 512 bytes).
    """
    listings: List[str] = []
    gd = Path(game_dir)
    for pattern in ("*.fst.txt", "*.fst.listing"):
        listings.extend(str(p) for p in gd.glob(pattern))
    # Check bare .fst files: skip binary archives
    for p in gd.glob("*.fst"):
        try:
            with open(str(p), "rb") as f:
                header = f.read(512)
            if b"\x00" not in header:
                listings.append(str(p))
        except OSError:
            pass
    return listings


# ---------------------------------------------------------------------------
# Numeric-size-subdir strip -- mirrors File::open strip loop (file.cpp:788-806)
# ---------------------------------------------------------------------------

def _strip_size_subdir(key: str) -> Optional[str]:
    """
    Strip a numeric size component from a path.
    e.g. 'data/tgl/128/foo.tga' -> 'data/tgl/foo.tga'
    Returns None if no numeric subdir is found.
    Mirrors the loop at file.cpp:789-805 exactly: scan for '/' then check if
    the next segment is all-digits followed by '/'; strip the first such match.
    """
    parts = key.split("/")
    for i, part in enumerate(parts[:-1]):  # last part is filename, never stripped
        if part.isdigit() and i > 0:
            stripped = "/".join(parts[:i] + parts[i + 1:])
            return stripped
    return None


# ---------------------------------------------------------------------------
# Config loader
# ---------------------------------------------------------------------------

class ResolverConfig:
    """
    Holds the resolution context for a single session.
    Construct via load_config() or directly.
    """
    def __init__(
        self,
        game_dir: str,
        mods_root: Optional[str] = None,
        active_mod: Optional[str] = None,
        fst_files: Optional[List[str]] = None,
    ) -> None:
        self.game_dir = game_dir.replace("\\", "/").rstrip("/")
        self.mods_root = (mods_root or os.path.join(game_dir, "mods")).replace("\\", "/")
        self.active_mod = active_mod  # None = base-game mode

        # mod_index: normalized_key -> (abs_path, mod_id)
        # shadowed:  normalized_key -> [mod_ids that lost]
        self.mod_index: Dict[str, Tuple[str, str]] = {}
        self.shadowed: Dict[str, List[str]] = {}

        # FastFile membership set
        if fst_files is None:
            fst_files = _discover_fst_listings(game_dir)
        self.fst_set: set[str] = _build_fst_set(fst_files)

        # Build mod index if a mod is active
        if active_mod:
            self._build_index()

    def _build_index(self) -> None:
        """
        Mirrors InitModSearchPaths (file.cpp:486-565):
        index active mod first, then dep[0], dep[1], ... dep[N-1].
        """
        assert self.active_mod
        mods_root = self.mods_root.rstrip("/")
        mod_dir = f"{mods_root}/{self.active_mod}"
        json_path = f"{mod_dir}/mod.json"

        mod_id_from_json, _, deps = _read_mod_json(json_path)
        # If mod.json missing or id field empty, fall back to folder name (file.cpp:516-521)
        effective_id = mod_id_from_json if mod_id_from_json else self.active_mod

        # Index active mod (highest priority)
        active_data = f"{mod_dir}/data/"
        _index_mod_layer(self.mod_index, self.shadowed, active_data, effective_id)

        # Index dependencies in declared order (dep[0] wins over dep[1], etc.)
        for dep_id in deps:
            dep_data = f"{mods_root}/{dep_id}/data/"
            _index_mod_layer(self.mod_index, self.shadowed, dep_data, dep_id)


def load_config(
    game_dir: str,
    mods_root: Optional[str] = None,
    active_mod: Optional[str] = None,
    fst_files: Optional[List[str]] = None,
) -> ResolverConfig:
    """
    Build a ResolverConfig from explicit parameters.
    If active_mod is None, reads MC2_ACTIVE_MOD from the environment.
    """
    if active_mod is None:
        active_mod = os.environ.get("MC2_ACTIVE_MOD") or None
    return ResolverConfig(
        game_dir=game_dir,
        mods_root=mods_root,
        active_mod=active_mod,
        fst_files=fst_files,
    )


# ---------------------------------------------------------------------------
# Core resolve function
# ---------------------------------------------------------------------------

def resolve(req: str, config: ResolverConfig) -> dict:
    """
    Resolve a single path request to a JSONL-schema record dict.

    Parameters
    ----------
    req:    Original path string as passed to File::open (before normalization).
    config: ResolverConfig built by load_config() or ResolverConfig().

    Returns
    -------
    dict matching the S3/S4 JSONL schema:
      {
        "v":        1,
        "key":      <normalized key>,
        "req":      <original req>,
        "layer":    <layer name>,
        "path":     <absolute path or "">,
        "shadowed": [<layer>, ...]
      }

    Layer names match the engine S3 trace exactly:
      "mod:<id>", "base-loose", "base-strip", "fastfile", "cd", "MISS"
    """
    # Normalize (mirrors file.cpp:743-752 + NormalizeKey:105)
    key = normalize_key(req)

    def _result(layer: str, path: str, shadowed_layers: Optional[List[str]] = None) -> dict:
        return {
            "v": 1,
            "key": key,
            "req": req,
            "layer": layer,
            "path": path,
            "shadowed": shadowed_layers if shadowed_layers is not None else [],
        }

    # Mod overlay (mirrors TryModOpen file.cpp:412-431)
    if config.mod_index and should_search_mods(key):
        if key in config.mod_index:
            abs_path, mod_id = config.mod_index[key]
            # Build shadowed list: list of layer names (mod:<id> or base-loose etc.)
            # The shadowed map records mod ids that lost at index time.
            shadowed_list = [f"mod:{sid}" for sid in config.shadowed.get(key, [])]
            # Also check if the base-loose file exists (it would be shadowed too)
            base_path = os.path.join(config.game_dir, key.replace("/", os.sep))
            if os.path.isfile(base_path):
                shadowed_list.append("base-loose")
            return _result(f"mod:{mod_id}", abs_path, shadowed_list)
        # mod-miss: fall through to base

    # Base loose file (mirrors file.cpp:780)
    base_path = os.path.join(config.game_dir, key.replace("/", os.sep))
    if os.path.isfile(base_path):
        return _result("base-loose", base_path.replace("\\", "/"))

    # Numeric-size-subdir strip (mirrors file.cpp:788-806)
    stripped = _strip_size_subdir(key)
    if stripped is not None:
        stripped_path = os.path.join(config.game_dir, stripped.replace("/", os.sep))
        if os.path.isfile(stripped_path):
            return _result("base-strip", stripped_path.replace("\\", "/"))

    # FastFile membership (mirrors FastFileFind file.cpp:814)
    if key in config.fst_set:
        return _result("fastfile", "")

    # CD path -- unreachable in modern deploys (Environment.checkCDForFiles=false)
    # Not simulated here; would need a CD install path parameter.
    # The skip-list in parity_smoke.py documents this as an acceptable divergence.

    # MISS
    return _result("MISS", "")


# ---------------------------------------------------------------------------
# Batch helpers
# ---------------------------------------------------------------------------

def resolve_many(reqs: List[str], config: ResolverConfig) -> List[dict]:
    """Resolve a list of path strings and return a list of records."""
    return [resolve(req, config) for req in reqs]


def resolve_trace(trace_records: List[dict], config: ResolverConfig) -> List[dict]:
    """
    Re-resolve every key from an engine trace (list of parsed JSONL dicts).
    Uses the 'req' field when present, else 'key'.
    Returns list of static-resolver records in the same order.
    """
    results = []
    for rec in trace_records:
        req = rec.get("req") or rec.get("key", "")
        results.append(resolve(req, config))
    return results
