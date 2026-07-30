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

DATA-PROVENANCE-CLOSE-1 (2026-07-30)
------------------------------------
This module used to PROBE and RETURN THE FIRST HIT, unverified. That is the
same defect STOCK-DATA-TRUTH-1 found in the mission-data tools: the failure is
silent, because a stale or locally-edited checkout still contains files that
parse and produce plausible numbers. Nothing here could tell you whether the
`.ini` it just read was the one the engine loads.

Every dir returned by `find_tgl_dir` / `find_srcdata` is now byte-verified
against the shipped `tgl.fst` (`tools/mc2data/stock_corpus.py`) before it is
returned, and an unverifiable dir is REFUSED. The probe ORDER is unchanged --
this is not a reordering fix. A working checkout is safe to probe because it is
checked, not because it is trusted.

Two escape hatches, both loud: `verify=False` (wire it to a `--allow-nonstock`
flag for modded/authored assets) and `$MC2_STOCK_DATA_ALLOW_NONSTOCK=1`.

CAVEAT, stated because it is easy to over-read the word "verified": the shipped
`tgl.fst` contains 2030 `.tgl` + 750 `.agl` compiled shapes and 773 `.ini` +
659 `128/*.tga`. The 2947 `.ase` authoring meshes in a source checkout have NO
shipped counterpart, so no manifest can prove a `.ase` equals what renders.
Verification here proves THE TREE is the stock tree (via its 1432 shipped
members); it does not prove any individual `.ase`.
"""
import difflib
import os
import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]

sys.path.insert(0, str(REPO_ROOT / "tools" / "mc2data"))
from stock_corpus import (  # noqa: E402
    SRCDATA_ROOT_PROBES as _SRCDATA_ROOT_PROBES,
    StockDataError,
    resolve as _resolve_corpus,
)

# Probe order lives in tools/mc2data/stock_corpus.py so exactly ONE file in
# this repo names stock-data directories in executable code (enforced by
# scripts/check-stock-mission-data.py). Order is unchanged from the historical
# union of asset_hd.py's and sweep.py's lists, plus the pristine extract.
SRCDATA_ROOT_PROBES = tuple(Path(p) for p in _SRCDATA_ROOT_PROBES)


class AssetResolutionError(Exception):
    pass


def _verify_tgl(tgl_dir, *, verify=True, quiet=False):
    """Prove `tgl_dir` is the stock tgl tree, or raise RuntimeError.

    Returns the (possibly resolver-substituted) directory. `verify=False`
    passes it straight through with a loud stderr banner.
    """
    try:
        res = _resolve_corpus("tgl", explicit=tgl_dir, allow_nonstock=not verify,
                              quiet=quiet)
    except StockDataError as e:
        raise RuntimeError(
            f"{e}\n"
            "  (DATA-PROVENANCE-CLOSE-1: an unverified srcdata tree is refused "
            "rather than silently used. Pass the modded-asset escape hatch, or "
            "set MC2_STOCK_DATA_ALLOW_NONSTOCK=1, if that is deliberate.)") from None
    return Path(res.dir)


def _first_verified(candidates, *, verify=True, quiet=False):
    """First candidate that EXISTS and VERIFIES; falls back to the resolver's
    own ladder (env override, pristine candidate, extraction from the deployed
    tgl.fst) when none does."""
    cands = [str(c) for c in candidates]
    try:
        res = _resolve_corpus("tgl", candidates=cands,
                              allow_nonstock=not verify, quiet=quiet)
    except StockDataError as e:
        raise RuntimeError(
            f"{e}\n  Probed, in order:\n  " + "\n  ".join(cands)) from None
    return Path(res.dir)


# ---------------------------------------------------------------------------
# Root/tgl-dir resolution
# ---------------------------------------------------------------------------

def find_tgl_dir(explicit=None, *, verify=True, quiet=False):
    """Return the VERIFIED stock tgl/ dir holding *.ase + *.ini, or raise.

    `explicit` may point at the srcdata root (containing tgl/) or at the tgl
    dir itself. Falls back to the MC2_SRCDATA env var, then the probe list.
    Whatever is chosen is byte-verified against the shipped tgl.fst.
    """
    if explicit:
        p = Path(explicit)
        if (p / "tgl").is_dir():
            return _verify_tgl(p / "tgl", verify=verify, quiet=quiet)
        if p.is_dir():
            return _verify_tgl(p, verify=verify, quiet=quiet)
        raise RuntimeError(f"--srcdata-root {explicit!r} is not a directory")

    cands = []
    env = os.environ.get("MC2_SRCDATA")
    if env:
        p = Path(env)
        cands.append(p / "tgl" if (p / "tgl").is_dir() else p)
    cands += [c / "tgl" for c in SRCDATA_ROOT_PROBES]
    return _first_verified(cands, verify=verify, quiet=quiet)


def find_srcdata(explicit=None, *, verify=True, quiet=False):
    """sweep.py-shaped entry point: same resolution, RuntimeError text kept
    close to sweep.py's historical wording (tests may match on it)."""
    if explicit:
        p = Path(explicit)
        if p.is_dir():
            return _verify_tgl(p, verify=verify, quiet=quiet)
        raise RuntimeError(f"--srcdata {explicit!r} is not a directory.")

    cands = []
    env = os.environ.get("MC2_SRCDATA")
    if env:
        cands.append(Path(env) / "tgl")
    cands += [c / "tgl" for c in SRCDATA_ROOT_PROBES]
    try:
        return _first_verified(cands, verify=verify, quiet=quiet)
    except RuntimeError as e:
        raise RuntimeError(
            "Could not locate the stock mc2srcdata/tgl directory. Pass "
            f"--srcdata.\n{e}") from None


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
