#!/usr/bin/env python3
"""
srcdata.py — BLENDER-PIPELINE-UNIFY-1 shared stock-srcdata resolver.

asset_hd.py, sweep.py, asset_edit.py and tex_hd.py each independently probed
for the mc2srcdata checkout and resolved asset names to files, with
duplicated/inconsistent logic (different probe lists, different error text).
This module is the single source of truth for:

  - locating the stock srcdata tgl/ dir (probe order below; explicit arg or
    the MC2_SRCDATA env var wins over probing)
  - listing/resolving asset names against that dir (*.ase + matching *.ini)

Callers with an "explicit root-or-tgl-dir" CLI flag (asset_hd.py: --srcdata-
root, sweep.py: --srcdata) use `find_tgl_dir`/`find_srcdata`. asset_edit.py's
`find_stock_asset` (single asset, srcdata ROOT not tgl dir, MC2_SRCDATA env)
keeps its own thin wrapper since its call shape (root + asset name in one
call, no tgl-dir concept) isn't shared by the others.
"""
import difflib
import os
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]

# Union of every probe entry previously duplicated across asset_hd.py and
# sweep.py, most-specific (repo-relative) first, de-duplicated, in the order
# a checkout is most likely to actually have the data.
SRCDATA_ROOT_PROBES = (
    REPO_ROOT / "mc2srcdata",
    REPO_ROOT / "mc2-srcdata",
    REPO_ROOT.parent / "mc2-srcdata",
    REPO_ROOT.parent / "mc2srcdata",
    Path("A:/Games/mc2-srcdata"),
    Path("A:/Games/mc2-opengl-src/mc2srcdata"),
)


class AssetResolutionError(Exception):
    pass


# ---------------------------------------------------------------------------
# Root/tgl-dir resolution
# ---------------------------------------------------------------------------

def find_tgl_dir(explicit=None):
    """Return the stock tgl/ dir holding *.ase + *.ini, or raise RuntimeError.

    `explicit` may point at the srcdata root (containing tgl/) or at the tgl
    dir itself. Falls back to the MC2_SRCDATA env var, then the probe list.
    """
    if explicit:
        p = Path(explicit)
        if (p / "tgl").is_dir():
            return p / "tgl"
        if p.is_dir():
            return p
        raise RuntimeError(f"--srcdata-root {explicit!r} is not a directory")

    env = os.environ.get("MC2_SRCDATA")
    if env:
        p = Path(env)
        if (p / "tgl").is_dir():
            return p / "tgl"
        if p.is_dir():
            return p

    for cand in SRCDATA_ROOT_PROBES:
        if (cand / "tgl").is_dir():
            return cand / "tgl"
    raise RuntimeError(
        "Could not locate the stock srcdata tgl dir. Pass --srcdata-root. Probed:\n  "
        + "\n  ".join(str(c) for c in SRCDATA_ROOT_PROBES))


def find_srcdata(explicit=None):
    """sweep.py-shaped entry point: same resolution, RuntimeError text kept
    close to sweep.py's historical wording (tests may match on it)."""
    if explicit:
        p = Path(explicit)
        if p.is_dir():
            return p
        raise RuntimeError(f"--srcdata {explicit!r} is not a directory.")

    env = os.environ.get("MC2_SRCDATA")
    if env:
        p = Path(env) / "tgl"
        if p.is_dir():
            return p

    for cand in SRCDATA_ROOT_PROBES:
        tgl = cand / "tgl"
        if tgl.is_dir():
            return tgl
    raise RuntimeError(
        "Could not locate the stock mc2srcdata/tgl directory. Pass --srcdata. "
        "Probed:\n  " + "\n  ".join(str(c / "tgl") for c in SRCDATA_ROOT_PROBES))


# ---------------------------------------------------------------------------
# Asset listing / resolution (pure)
# ---------------------------------------------------------------------------

def list_assets(tgl_dir, substring=""):
    """Sorted asset stems whose name contains `substring` (case-insensitive)."""
    want = substring.lower()
    return sorted((p.stem for p in Path(tgl_dir).glob("*.ase")
                   if want in p.stem.lower()), key=str.lower)


def resolve_asset(name, tgl_dir):
    """Return (stem, ase_path, ini_path) for a case-insensitive asset name.

    Raises AssetResolutionError with closest-match suggestions when the asset
    (or its mandatory stock .ini) is missing.
    """
    tgl_dir = Path(tgl_dir)
    want = name.lower()
    if want.endswith(".ase"):
        want = want[:-4]

    by_lower = {}
    for p in sorted(tgl_dir.glob("*.ase")):
        by_lower.setdefault(p.stem.lower(), p)
    ase = by_lower.get(want)
    if ase is None:
        stems = sorted(by_lower)
        close = difflib.get_close_matches(want, stems, n=6, cutoff=0.6)
        contains = [s for s in stems if want in s][:6]
        sugg = list(dict.fromkeys(close + contains))[:8]
        hint = f"; closest: {', '.join(sugg)}" if sugg else ""
        raise AssetResolutionError(
            f"asset {name!r} not found in {tgl_dir}{hint}")

    ini = {p.stem.lower(): p for p in tgl_dir.glob("*.ini")}.get(want)
    if ini is None:
        raise AssetResolutionError(
            f"asset {name!r}: no matching .ini for {ase.name} in {tgl_dir} "
            "(the ase->glb extractor needs the stock ini)")
    return ase.stem, ase, ini


def enumerate_assets(tgl_dir):
    """Return sorted [(stem, ase_path, ini_path)] for every .ase with a
    matching .ini (case-insensitive stem match — Windows filesystem reality).
    """
    tgl_dir = Path(tgl_dir)
    inis = {p.stem.lower(): p for p in tgl_dir.glob("*.ini")}
    out = []
    for ase in tgl_dir.glob("*.ase"):
        ini = inis.get(ase.stem.lower())
        if ini is not None:
            out.append((ase.stem, ase, ini))
    out.sort(key=lambda t: t[0].lower())
    return out
