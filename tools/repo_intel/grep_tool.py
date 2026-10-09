"""
REPO-INTEL-1d: grep_tool.py
Lexical grep over the MC2 source tree.

Provides:
  grep_source(root, pattern, ...)  — pattern search with default excludes
  symbol_lookup(root, symbol, ...) — best-effort definition/reference split

Both return confidence="lexical".
NOT clangd, NOT AST, NOT graphify. Heuristics only.
"""

import fnmatch
import os
import re
import subprocess
from pathlib import Path
from typing import Optional
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

# ---------------------------------------------------------------------------
# Default excludes
# ---------------------------------------------------------------------------

_EXCLUDE_DIRS = {
    ".git", "build64", "releases", "3rdparty", ".claude",
    "dist", ".vscode", ".clangd",
}

# tests/smoke/artifacts is excluded but not all of tests/
_EXCLUDE_DIR_PATHS = {
    "tests/smoke/artifacts",
}

_EXCLUDE_FILE_GLOBS = [
    "*.log", "*.jsonl", "*.pdb", "*.obj", "*.lib",
    "*.dll", "*.exe", "*.zip",
    "*.ktx2", "*.dds", "*.png", "*.tga", "*.jpg", "*.bmp",
    "*.glb", "*.fbx", "*.wav", "*.pak", "*.fst",
    "*.csv", "*.bin",
]

_MAX_FILE_BYTES = 2 * 1024 * 1024   # skip files >2 MB


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _dir_excluded(rel: str, extra: list) -> bool:
    """Return True if this relative dir path should be pruned."""
    parts = Path(rel).parts
    for p in parts:
        if p in _EXCLUDE_DIRS:
            return True
    norm = rel.replace("\\", "/")
    for ep in _EXCLUDE_DIR_PATHS:
        if norm.startswith(ep):
            return True
    for g in extra:
        if fnmatch.fnmatch(norm, g) or fnmatch.fnmatch(Path(rel).name, g):
            return True
    return False


def _file_excluded(name: str, extra: list) -> bool:
    for g in _EXCLUDE_FILE_GLOBS + extra:
        if fnmatch.fnmatch(name, g):
            return True
    return False


def _file_included(name: str, include_globs: list) -> bool:
    if not include_globs:
        return True
    return any(fnmatch.fnmatch(name, g) for g in include_globs)


# ---------------------------------------------------------------------------
# rg fast path (optional)
# ---------------------------------------------------------------------------

def _try_rg(
    root: Path,
    pattern: str,
    include_globs: list,
    exclude_globs: list,
    case_sensitive: bool,
    max_results: int,
) -> Optional[dict]:
    """Try ripgrep. Returns None if rg is unavailable."""
    cmd = ["rg", "--line-number", "--no-heading", "--color=never"]
    if not case_sensitive:
        cmd.append("--ignore-case")
    cmd += ["--max-count", "1"]            # per-file cap (we cap total below)
    cmd += ["--max-filesize", "2M"]

    # Default dir excludes
    for d in _EXCLUDE_DIRS:
        cmd += ["--glob", f"!{d}/**"]
    for dp in _EXCLUDE_DIR_PATHS:
        cmd += ["--glob", f"!{dp}/**"]
    # Default file excludes
    for g in _EXCLUDE_FILE_GLOBS:
        cmd += ["--glob", f"!{g}"]
    # Extra excludes from caller
    for g in exclude_globs:
        cmd += ["--glob", f"!{g}"]
    # Include globs
    for g in include_globs:
        cmd += ["--glob", g]

    cmd += ["-e", pattern, str(root)]

    try:
        r = subprocess.run(
            cmd, capture_output=True, text=True, timeout=15,
            stdin=subprocess.DEVNULL,
            **nofocus.hidden_kwargs()
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None

    matches = []
    truncated = False
    for raw in r.stdout.splitlines():
        if len(matches) >= max_results:
            truncated = True
            break
        # rg --no-heading format:  path:lineno:snippet
        parts = raw.split(":", 2)
        if len(parts) < 3:
            continue
        fpath, lineno_s, snippet = parts
        rel = Path(fpath).relative_to(root).as_posix()
        try:
            lineno = int(lineno_s)
        except ValueError:
            lineno = 0
        matches.append({"file": rel, "line": lineno, "snippet": snippet.rstrip()})

    return {
        "matches": matches,
        "match_count": len(matches),
        "truncated": truncated,
        "confidence": "lexical",
    }


# ---------------------------------------------------------------------------
# Python fallback
# ---------------------------------------------------------------------------

def _py_grep(
    root: Path,
    pattern: str,
    include_globs: list,
    exclude_globs: list,
    case_sensitive: bool,
    max_results: int,
) -> dict:
    flags = 0 if case_sensitive else re.IGNORECASE
    try:
        rx = re.compile(pattern, flags)
    except re.error as e:
        return {"error": f"invalid regex: {e}", "confidence": "lexical"}

    matches = []
    truncated = False
    done = False

    for dirpath, dirnames, filenames in os.walk(root):
        if done:
            break
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir == ".":
            rel_dir = ""

        dirnames[:] = [
            d for d in dirnames
            if not _dir_excluded(
                os.path.join(rel_dir, d) if rel_dir else d,
                exclude_globs,
            )
        ]

        for fname in filenames:
            if done:
                break
            if not _file_included(fname, include_globs):
                continue
            if _file_excluded(fname, exclude_globs):
                continue

            fpath = os.path.join(dirpath, fname)
            rel = (os.path.join(rel_dir, fname) if rel_dir else fname).replace("\\", "/")

            try:
                if os.path.getsize(fpath) > _MAX_FILE_BYTES:
                    continue
                with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                    for lineno, line in enumerate(f, 1):
                        if rx.search(line):
                            matches.append({
                                "file": rel,
                                "line": lineno,
                                "snippet": line.rstrip(),
                            })
                            if len(matches) >= max_results:
                                truncated = True
                                done = True
                                break
            except (OSError, PermissionError):
                continue

    return {
        "matches": matches,
        "match_count": len(matches),
        "truncated": truncated,
        "confidence": "lexical",
    }


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def grep_source(
    root: Path,
    pattern: str,
    include_globs: Optional[list] = None,
    exclude_globs: Optional[list] = None,
    case_sensitive: bool = True,
    max_results: int = 200,
    mode: str = "content",
) -> dict:
    """
    Grep the source tree for pattern.

    Default excludes: .git build64 releases 3rdparty .claude
                      tests/smoke/artifacts *.log *.jsonl binaries

    mode="content" (default): {matches, match_count, truncated, confidence}
    mode="files": {files, file_count, match_count, truncated, confidence} (no snippets)
    mode="count": {match_count, file_count, truncated, confidence} (no snippets)
    """
    inc = list(include_globs or [])
    exc = list(exclude_globs or [])

    # Validate pattern early (same error either way)
    try:
        re.compile(pattern)
    except re.error as e:
        return {"error": f"invalid regex: {e}", "confidence": "lexical"}

    result = _try_rg(root, pattern, inc, exc, case_sensitive, max_results)
    if result is None:
        result = _py_grep(root, pattern, inc, exc, case_sensitive, max_results)
    if "error" in result:
        return result

    if mode == "content":
        return result

    files = sorted({m["file"] for m in result["matches"]})
    if mode == "files":
        return {
            "files":       files,
            "file_count":  len(files),
            "match_count": result["match_count"],
            "truncated":   result["truncated"],
            "confidence":  "lexical",
        }
    if mode == "count":
        return {
            "match_count": result["match_count"],
            "file_count":  len(files),
            "truncated":   result["truncated"],
            "confidence":  "lexical",
        }
    return result


# ---------------------------------------------------------------------------
# Definition heuristics for symbol_lookup
# ---------------------------------------------------------------------------

def _is_definition(line: str, esc: str) -> bool:
    """
    Heuristic: does this line look like a definition/declaration of the symbol?

    Errs toward false-negative (missed def) rather than false-positive
    (call misclassified as def).  Callers see all hits anyway.
    """
    _DEF_RX = re.compile(
        rf"(?:"
        # class/struct/enum/namespace declaration
        rf"\b(?:class|struct|enum|typedef|using|namespace)\s+{esc}\b"
        # typed declaration: known type keyword then symbol
        rf"|\b(?:void|bool|int|float|double|char|auto|DWORD|HGOS\w*|TG_\w+|gos\w+"
        rf"|MC2\w*|Gos\w+|static|inline|explicit|virtual|override|const)\s"
        rf"(?:.*\s)?{esc}\b"
        # scope-qualified implementation: Foo::Symbol(  or Foo::Symbol =
        rf"|\w+::{esc}\s*[({{]"
        rf"|\w+::{esc}\s*="
        # #define SYMBOL
        rf"|#define\s+{esc}\b"
        rf")"
    )
    return bool(_DEF_RX.search(line))


def symbol_lookup(
    root: Path,
    symbol: str,
    max_results: int = 300,
    in_ref: str = "",
    def_context: int = 0,
) -> dict:
    """
    Best-effort lexical symbol lookup.

    Searches for \\bsymbol\\b, then splits hits into definition-candidates
    and references using C++ heuristics.

    in_ref (e.g. "HEAD"): reports whether the symbol is already present in that
        git ref — answers "is this new vs existing".
    def_context > 0: attaches N lines of body context to the first definition.

    NOT clangd, NOT AST, NOT graphify.
    Definition heuristics may misclassify — verify with Read tool.
    """
    if not symbol.strip():
        return {"error": "symbol must not be empty", "confidence": "lexical"}

    esc = re.escape(symbol)
    all_hits = grep_source(root, rf"\b{esc}\b", max_results=max_results)
    if "error" in all_hits:
        return all_hits

    definitions = []
    references = []
    for m in all_hits["matches"]:
        (definitions if _is_definition(m["snippet"], esc) else references).append(m)

    caller_files = sorted({m["file"] for m in references})

    result = {
        "symbol":       symbol,
        "definitions":  definitions,
        "references":   references,
        "caller_files": caller_files,
        "match_count":  all_hits["match_count"],
        "truncated":    all_hits["truncated"],
        "confidence":   "lexical",
        "note":         "Lexical only — not clangd, not AST, not graphify. "
                        "Definition heuristics are best-effort; verify with Read.",
    }

    # in_ref: presence-in-ref check via fixed-string git grep.
    if in_ref:
        present = False
        match_count = 0
        try:
            r = subprocess.run(
                ["git", "-C", str(root), "grep", "-n", "-F", symbol, in_ref],
                capture_output=True, text=True, timeout=15,
                stdin=subprocess.DEVNULL,
                **nofocus.hidden_kwargs()
            )
            lines = [l for l in r.stdout.splitlines() if l.strip()]
            match_count = len(lines)
            present = r.returncode == 0 and match_count > 0
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            present = False
            match_count = 0
        result["in_ref"] = {
            "ref":         in_ref,
            "present":     present,
            "match_count": match_count,
        }

    # def_context: attach body context to the first definition.
    if def_context > 0 and definitions:
        ctx_n = min(def_context, 60)
        d0 = definitions[0]
        try:
            fpath = Path(root) / d0["file"]
            with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                file_lines = f.readlines()
            start = max(0, d0["line"] - 1)
            end = min(len(file_lines), start + ctx_n)
            d0["context"] = [
                f"{i + 1}: {file_lines[i].rstrip()}"
                for i in range(start, end)
            ]
        except (OSError, KeyError):
            pass

    return result
