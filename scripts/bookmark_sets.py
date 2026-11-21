"""One resolver for capture bookmark sets, shared by tools/visual_verify/ab.py and capture.py.

A set is named by a bare stem or a full .json path. A stem is searched in order: <install>/saved_bookmarks, the repo's
tests/visual/bookmarks, then the ops dir (env MC2_OPS_BOOKMARKS, default A:/Games/mc2-ops/bookmarks; a missing dir is skipped).
"""
from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_BOOKMARKS = ROOT / "tests" / "visual" / "bookmarks"
DEFAULT_OPS_BOOKMARKS = "A:/Games/mc2-ops/bookmarks"  # drive-path: ok ops-only default; env MC2_OPS_BOOKMARKS overrides, a missing dir is skipped


def ops_bookmarks_dir() -> Path:
    return Path(os.environ.get("MC2_OPS_BOOKMARKS") or DEFAULT_OPS_BOOKMARKS)


def is_path_spec(spec: str) -> bool:
    return spec.lower().endswith(".json") or "/" in spec or "\\" in spec


def spec_stem(spec: str) -> str:
    """The set name a spec (stem or .json path) captures under."""
    spec = spec.strip()
    return Path(spec).stem if is_path_spec(spec) else spec


def search_dirs(install=None, repo_dir=None, ops_dir=None) -> "list[Path]":
    dirs = []
    if install is not None:
        dirs.append(Path(install) / "saved_bookmarks")
    dirs.append(REPO_BOOKMARKS if repo_dir is None else Path(repo_dir))
    dirs.append(ops_bookmarks_dir() if ops_dir is None else Path(ops_dir))
    return dirs


def resolve_set(spec: str, install=None, repo_dir=None, ops_dir=None) -> "tuple[Path | None, list[Path]]":
    """(file or None, dirs searched). A .json path that exists is used as is; otherwise the stem is looked up in each dir."""
    spec = spec.strip()
    if is_path_spec(spec) and Path(spec).is_file():
        return Path(spec), []
    dirs = search_dirs(install, repo_dir, ops_dir)
    stem = spec_stem(spec)
    for d in dirs:
        if d.is_dir() and (d / (stem + ".json")).is_file():
            return d / (stem + ".json"), dirs
    return None, dirs


def not_found_message(spec: str, dirs) -> str:
    where = "; ".join(str(d) for d in dirs) or "(none)"
    return f"bookmark set {spec!r} not found (searched: {where})"
