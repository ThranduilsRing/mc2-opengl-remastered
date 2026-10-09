#!/usr/bin/env python3
"""
tex_hd.py — BLENDER-TEX-UPSCALE-WIRE-1 per-asset texture upscale.

Finds the stock textures an asset references (via its .ini shape list +
.ase *BITMAP records under mc2srcdata), upscales each one through the
EXISTING mc2texcook machinery (cook_tgl_upscale_single.py: PIL Lanczos
resize + ktx.exe uastc->BC7 KTX2 cook — the same pipeline that produced
the shipped fireantrgb/a_hangar cooks), and emits engine-consumable
.ktx2 files.

Engine pickup seams (both real, both gated):
  1. SIDECAR (proven, shipped precedent fireantrgb.ktx2 512x512 in
     data/tgl/128/): place <stem>.ktx2 NEXT TO the matching <stem>.tga
     (e.g. <install>/data/tgl/128/a_quonset.ktx2). Consumed by
     mclib/txmmgr.cpp (TEXMGR-COMPRESSED-UPLOAD-1 / KTX_PRIMARY) —
     needs MC2_TEXMGR_COMPRESSED_UPLOAD=1.
  2. TIER DIR: <install>/data/tgl/<tier>/<stem>.ktx2 where tier is one
     of 128/256/512/1024. Consumed by gos_static_prop_batcher.cpp
     deriveTieredKtxPath() — tier selected by MC2_STATICPROP_TEX_TIER
     (engine default 512); needs the material-KTX gate on.

Usage:
    py -3 tools/blender_pipeline/tex_hd.py quonset [hangar ...]
        [--scale N]        integer upscale factor (default 4, mc2texcook's
                           proven default; 128 -> 512)
        [--out-dir DIR]    output root (default tools/blender_pipeline/out/
                           <asset>/textures/)
        [--deploy-dir DIR] optional install root (e.g. a DEV lane). No
                           default on purpose — never auto-inject into the
                           user-launch lane. Copies each .ktx2 as a sidecar
                           next to the matching .tga under data/tgl/, or
                           into data/tgl/<tier>/ when no .tga is found.
        [--ase-root DIR]   stock .ase/.ini root. NO baked-in default
                           (DATA-PROVENANCE-CLOSE-1): omit it and the verified
                           resolver in tools/mc2data/stock_corpus.py locates a
                           tree and PROVES it matches the shipped tgl.fst.
                           Whatever is passed is verified too.
        [--allow-nonstock] proceed on a tree that does NOT match the shipped
                           bytes (modded/authored assets). Loud banner.
        [--tga-root DIR]   source-TGA search root; repeatable; defaults to
                           <ase-root>/128 then <ase-root>
        [--tier N]         tier-dir name for seam 2 (default 128*scale)
        [--dry-run]        list textures found + planned commands, no writes

Exit codes: 0 ok, 1 one or more textures failed to cook, 2 usage/lookup error.

Stdlib only (the wrapped mc2texcook cook uses PIL + ktx.exe — its own,
pre-existing deps). Per-texture failures are collected, not fatal.
"""
from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
COOK_SINGLE = TOOL_DIR.parent / "mc2texcook" / "cook_tgl_upscale_single.py"

sys.path.insert(0, str(TOOL_DIR))
sys.path.insert(0, str(TOOL_DIR.parents[1] / "tools" / "mc2data"))
import srcdata as _srcdata  # noqa: E402
from stock_corpus import add_allow_nonstock_arg  # noqa: E402

# DATA-PROVENANCE-CLOSE-1: `--ase-root` used to default to the absolute path
# `A:/Games/mc2-srcdata/tgl` -- an EDITABLE checkout, taken on faith. It agrees
# with the shipped tgl.fst today; it was never checked, and nothing here could
# have told you if it stopped. There is no default now: the resolver finds a
# tree and proves it. See tools/mc2data/stock_corpus.py.
DEFAULT_SCALE = 4  # mc2texcook cook_tgl_upscale_single.py default (128 -> 512)

# .ase material record:  *BITMAP "\\Aas1\mc2\Art\...\A_Quonset.tga"
_BITMAP_RE = re.compile(r'\*BITMAP\s+"([^"]+)"')
# FitIni shape refs:  st FileName0="Quonset" / st ShadowName="QuonsetX"
_SHAPE_RE = re.compile(r'^\s*st\s+(?:FileName\d*|ShadowName)\s*=\s*"([^"]+)"',
                       re.MULTILINE)


# ---------------------------------------------------------------------------
# Pure helpers (pytest-covered, no file IO beyond what the caller hands in)
# ---------------------------------------------------------------------------

def parse_ase_bitmaps(text: str) -> list[str]:
    """Extract ordered unique texture basenames from .ase *BITMAP records.

    Paths in stock .ase are Windows/UNC (\\\\Aas1\\mc2\\Art\\...\\A_Quonset.tga);
    only the basename is meaningful — the engine resolves by name.
    """
    seen: set[str] = set()
    out: list[str] = []
    for raw in _BITMAP_RE.findall(text):
        name = raw.replace("\\", "/").rsplit("/", 1)[-1].strip()
        if not name:
            continue
        key = name.lower()
        if key not in seen:
            seen.add(key)
            out.append(name)
    return out


def parse_ini_shape_stems(text: str) -> list[str]:
    """Extract ordered unique shape stems (FileNameN / ShadowName) from a
    FitIni actor .ini ([TGLData]/[TGLDamage] blocks)."""
    seen: set[str] = set()
    out: list[str] = []
    for stem in _SHAPE_RE.findall(text):
        key = stem.lower()
        if key and key not in seen:
            seen.add(key)
            out.append(stem)
    return out


def find_case_insensitive(dirpath: Path, filename: str) -> Path | None:
    """Case-insensitive file lookup in a single directory (stock refs say
    A_Quonset.tga, disk says a_quonset.tga)."""
    if not dirpath.is_dir():
        return None
    exact = dirpath / filename
    if exact.is_file():
        return exact
    want = filename.lower()
    for p in dirpath.iterdir():
        if p.is_file() and p.name.lower() == want:
            return p
    return None


def tga_dims(path: Path) -> tuple[int, int] | None:
    """Width/height from a TGA header (bytes 12..15, little-endian u16)."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(18)
        if len(hdr) < 18:
            return None
        w, h = struct.unpack_from("<HH", hdr, 12)
        return (w, h)
    except OSError:
        return None


_KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"


def ktx2_dims(path: Path) -> tuple[int, int] | None:
    """Width/height from a KTX2 header (pixelWidth/Height u32 at offset 20)."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(28)
        if len(hdr) < 28 or hdr[:12] != _KTX2_MAGIC:
            return None
        w, h = struct.unpack_from("<II", hdr, 20)
        return (w, h)
    except OSError:
        return None


def build_cook_cmd(src: Path, dst: Path, scale: int,
                   python: str | None = None) -> list[str]:
    """Command line for one texture through the existing mc2texcook single
    cook (PIL Lanczos upscale + uastc->BC7 KTX2)."""
    return [
        python or sys.executable, str(COOK_SINGLE),
        "--src", str(src),
        "--dst", str(dst),
        "--scale", str(scale),
    ]


# ---------------------------------------------------------------------------
# Asset -> texture-ref resolution
# ---------------------------------------------------------------------------

def collect_texture_refs(asset: str, ase_root: Path) -> tuple[list[str], list[str]]:
    """Return (texture basenames, notes) for one asset.

    Shape list comes from <asset>.ini (FileNameN + ShadowName) when present,
    always unioned with the asset stem itself; each shape's .ase is scanned
    for *BITMAP records.
    """
    notes: list[str] = []
    stems: list[str] = [asset]
    ini_path = find_case_insensitive(ase_root, asset + ".ini")
    if ini_path:
        try:
            ini_text = ini_path.read_text(errors="ignore")
            for s in parse_ini_shape_stems(ini_text):
                if s.lower() not in {x.lower() for x in stems}:
                    stems.append(s)
            notes.append(f"ini: {ini_path.name} -> shapes {stems}")
        except OSError as exc:
            notes.append(f"ini read failed ({ini_path.name}): {exc}")
    else:
        notes.append(f"no {asset}.ini in {ase_root} — using bare .ase")

    refs: list[str] = []
    seen: set[str] = set()
    found_any_ase = False
    for stem in stems:
        ase_path = find_case_insensitive(ase_root, stem + ".ase")
        if not ase_path:
            notes.append(f"missing .ase for shape '{stem}'")
            continue
        found_any_ase = True
        try:
            text = ase_path.read_text(errors="ignore")
        except OSError as exc:
            notes.append(f".ase read failed ({ase_path.name}): {exc}")
            continue
        for name in parse_ase_bitmaps(text):
            if name.lower() not in seen:
                seen.add(name.lower())
                refs.append(name)
    if not found_any_ase:
        notes.append(f"no .ase found for asset '{asset}' under {ase_root}")
    return refs, notes


def locate_source_tga(ref: str, tga_roots: list[Path]) -> Path | None:
    for root in tga_roots:
        hit = find_case_insensitive(root, ref)
        if hit:
            return hit
    return None


# ---------------------------------------------------------------------------
# Deploy seam
# ---------------------------------------------------------------------------

def deploy_targets(deploy_dir: Path, stem: str, tier: int) -> tuple[Path, str]:
    """Where one cooked <stem>.ktx2 must land inside an install for engine
    pickup. Prefers the SIDECAR seam (next to the matching .tga); falls back
    to the tier-dir seam."""
    tgl = deploy_dir / "data" / "tgl"
    for cand_dir in (tgl / "128", tgl):
        tga = find_case_insensitive(cand_dir, stem + ".tga")
        if tga:
            return tga.with_suffix(".ktx2"), "sidecar"
    return tgl / str(tier) / (stem + ".ktx2"), "tier"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Per-asset texture upscale via the existing mc2texcook "
                    "machinery (PIL Lanczos + BC7 KTX2).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("assets", nargs="+", metavar="ASSET",
                    help="asset stems, e.g. quonset hangar")
    ap.add_argument("--scale", type=int, default=DEFAULT_SCALE,
                    help=f"integer upscale factor (default {DEFAULT_SCALE})")
    ap.add_argument("--out-dir", type=Path, default=None,
                    help="output root (default tools/blender_pipeline/out/"
                         "<asset>/textures/)")
    ap.add_argument("--deploy-dir", type=Path, default=None,
                    help="optional install root to copy cooked .ktx2 into "
                         "(NO default — never auto-targets the user-launch lane)")
    ap.add_argument("--ase-root", type=Path, default=None,
                    help="stock .ase/.ini root. Omit to auto-resolve and "
                         "byte-verify against the shipped tgl.fst; whatever "
                         "is passed is verified too.")
    ap.add_argument("--tga-root", type=Path, action="append", default=None,
                    help="source-TGA search root (repeatable; default "
                         "<ase-root>/128 then <ase-root>)")
    add_allow_nonstock_arg(ap)
    ap.add_argument("--tier", type=int, default=0,
                    help="tier-dir name for the tier seam (default 128*scale)")
    ap.add_argument("--dry-run", action="store_true",
                    help="list textures + planned commands, write nothing")
    args = ap.parse_args(argv)

    if args.scale < 1:
        print("ERROR: --scale must be >= 1", file=sys.stderr)
        return 2
    try:
        ase_root: Path = _srcdata.find_tgl_dir(args.ase_root,
                                               verify=not args.allow_nonstock)
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    tga_roots: list[Path] = args.tga_root or [ase_root / "128", ase_root]
    tier: int = args.tier or 128 * args.scale

    total_cooked = 0
    total_failed = 0
    failures: list[str] = []
    deployed: list[tuple[Path, str]] = []

    for asset in args.assets:
        out_dir: Path = (args.out_dir if args.out_dir
                         else TOOL_DIR / "out" / asset / "textures")
        print(f"=== {asset} ===")
        refs, notes = collect_texture_refs(asset, ase_root)
        for n in notes:
            print(f"  [recon] {n}")
        if not refs:
            print(f"  no texture refs found for '{asset}'", file=sys.stderr)
            total_failed += 1
            failures.append(f"{asset}: no texture refs")
            continue

        for ref in refs:
            stem = Path(ref).stem.lower()
            src = locate_source_tga(ref, tga_roots)
            if not src:
                print(f"  MISS {ref}: no source .tga under "
                      f"{[str(r) for r in tga_roots]}", file=sys.stderr)
                failures.append(f"{asset}/{ref}: source tga not found")
                total_failed += 1
                continue
            dims = tga_dims(src)
            src_res = f"{dims[0]}x{dims[1]}" if dims else "?"
            dst = out_dir / f"{stem}.ktx2"
            cmd = build_cook_cmd(src, dst, args.scale)
            if args.dry_run:
                print(f"  PLAN {ref}: {src} ({src_res}) "
                      f"-> {dst} (x{args.scale})")
                print(f"       cmd: {' '.join(cmd)}")
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            r = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if r.returncode != 0 or not dst.is_file():
                detail = (r.stderr or r.stdout).strip()[:300]
                print(f"  FAIL {ref}: cook rc={r.returncode}: {detail}",
                      file=sys.stderr)
                failures.append(f"{asset}/{ref}: cook failed")
                total_failed += 1
                continue
            out_dims = ktx2_dims(dst)
            out_res = f"{out_dims[0]}x{out_dims[1]}" if out_dims else "?"
            print(f"  OK   {ref}: {src_res} -> {out_res}  "
                  f"{dst}  ({dst.stat().st_size:,} bytes)")
            total_cooked += 1

            if args.deploy_dir:
                target, seam = deploy_targets(args.deploy_dir, stem, tier)
                prev = ktx2_dims(target) if target.is_file() else None
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dst, target)
                note = (f" (replaced {prev[0]}x{prev[1]})" if prev else "")
                print(f"       deployed [{seam}] -> {target}{note}")
                deployed.append((target, seam))

    print()
    print("Engine pickup (static props):")
    print("  seam 1 SIDECAR: <install>/data/tgl/128/<stem>.ktx2 next to the "
          ".tga — set MC2_TEXMGR_COMPRESSED_UPLOAD=1")
    print(f"  seam 2 TIER DIR: <install>/data/tgl/{tier}/<stem>.ktx2 — set "
          f"MC2_STATICPROP_TEX_TIER={tier} (+ material-KTX gate)")
    if args.deploy_dir and deployed and not args.dry_run:
        print(f"  deployed {len(deployed)} file(s) into {args.deploy_dir}")
    elif not args.deploy_dir:
        print("  (no --deploy-dir given: outputs left in --out-dir; copy "
              "manually or re-run with --deploy-dir <DEV lane>)")

    if failures:
        print(f"\nFailures ({len(failures)}):", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
    if not args.dry_run:
        print(f"\nDONE cooked={total_cooked} failed={total_failed}")
    return 1 if total_failed else 0


if __name__ == "__main__":
    sys.exit(main())
