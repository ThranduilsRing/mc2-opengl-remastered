#!/usr/bin/env python3
"""scripts/cook_upscaled_assets_mod.py -- Cook upscaled assets into a deployable mod.

Two asset classes:
  1. Terrain burnin colormaps (.burnin.tga from zip) -> .burnin.ktx2 BC7
     Engine probes for .burnin.ktx2 sidecar in terrtxm2.cpp:1613.
     Downscaled to --max-edge (default 4096) from 5120^2 source.

  2. GUI/art textures (from mc2-art.zip) -> loose TGA/PNG in mod data/art/.
     Engine loads art by exact filename; no .ktx2 probe exists for art.
     Shipped as-is (no cook, just extraction).

Output mod structure:
  <mod-dir>/
    mod.json
    data/
      textures/   <- .burnin.ktx2 files
      art/        <- TGA/PNG files from art zip

Usage:
  py -3 scripts/cook_upscaled_assets_mod.py [options]

  --mod-dir     DIR    Output mod directory (created if absent)
  --burnin-zips ZIP    Zip(s) containing .burnin.tga files (repeatable)
  --art-zip     ZIP    Zip containing GUI/art TGA/PNG files
  --missions    LIST   Comma-separated mission stems (default: mc2_01,mc2_03,mc2_10,mc2_17,mc2_24)
  --max-edge    N      Downscale burnins: cap longest edge (default: 4096)
  --ktx-tool    PATH   Path to ktx.exe
  --skip-existing      Skip files already in mod-dir
  --dry-run            Print actions, don't run
"""
from __future__ import annotations

import argparse
import io
import json
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not found. pip install pillow", file=sys.stderr)
    sys.exit(1)

_DEFAULT_KTX_TOOL = r"A:/Games/mc2-tools/ktx/ktx.exe"
_DEFAULT_MISSIONS = "mc2_01,mc2_03,mc2_10,mc2_17,mc2_24"

_MOD_JSON_TEMPLATE = {
    "schema": "mc2-mod/1",
    "id": "UpscaledAssets",
    "name": "Upscaled Assets (4x)",
    "version": "1.0.0",
    "type": "assets",
    "description": "4x ESRGAN upscaled terrain colormaps (4096^2 BC7) and GUI art (1024/512 TGA).",
    "dependencies": [],
}


def _cook_burnin_ktx2(img_rgba: Image.Image, dst: Path, ktx_tool: str,
                      max_edge: int, dry_run: bool, label: str) -> bool:
    """Downscale + cook a single RGBA burnin image -> BC7 KTX2."""
    src_w, src_h = img_rgba.size

    if max_edge > 0 and max(src_w, src_h) > max_edge:
        scale = max_edge / max(src_w, src_h)
        new_w = int(src_w * scale)
        new_h = int(src_h * scale)
        img_rgba = img_rgba.resize((new_w, new_h), Image.LANCZOS)
        print(f"  {label}: resized {src_w}x{src_h} -> {new_w}x{new_h}")
    else:
        new_w, new_h = src_w, src_h

    if dry_run:
        print(f"  [DRY] {label} {new_w}x{new_h} -> {dst.name}")
        return True

    dst.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        png_path = tdp / "src.png"
        uastc_path = tdp / "uastc.ktx2"

        img_rgba.save(png_path, format="PNG")

        create_cmd = [
            ktx_tool, "create",
            "--encode", "uastc",
            "--format", "R8G8B8A8_UNORM",
            "--assign-tf", "linear",
            "--levels", "1",
            str(png_path), str(uastc_path),
        ]
        r = subprocess.run(create_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  [FAIL] ktx create {label}: rc={r.returncode} "
                  f"{(r.stderr or r.stdout).strip()[:200]}", file=sys.stderr)
            return False

        transcode_cmd = [
            ktx_tool, "transcode", "--target", "bc7",
            str(uastc_path), str(dst),
        ]
        r = subprocess.run(transcode_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  [FAIL] ktx transcode {label}: rc={r.returncode} "
                  f"{(r.stderr or r.stdout).strip()[:200]}", file=sys.stderr)
            if dst.exists():
                dst.unlink()
            return False

    dst_kb = dst.stat().st_size // 1024
    print(f"  [OK] {label} {new_w}x{new_h} -> {dst.name} {dst_kb}KB")
    return True


def cook_burnins(burnin_zips: list[Path], missions: list[str], tex_dir: Path,
                 max_edge: int, ktx_tool: str, skip_existing: bool,
                 dry_run: bool) -> tuple[int, int, int]:
    """Extract .burnin.tga for each mission from zips, cook to .burnin.ktx2."""
    ok = skip = fail = 0

    for mission in missions:
        dst = tex_dir / f"{mission}.burnin.ktx2"
        if skip_existing and dst.exists():
            print(f"  [SKIP] {mission}.burnin.ktx2 exists")
            skip += 1
            continue

        # Find the .burnin.tga in any of the zips
        found = False
        for zp in burnin_zips:
            if not zp.is_file():
                continue
            with zipfile.ZipFile(zp) as zf:
                names = zf.namelist()
                match = next(
                    (n for n in names
                     if Path(n).name == f"{mission}.burnin.tga"),
                    None,
                )
                if match is None:
                    continue
                print(f"  {mission}: found in {zp.name}:{match}")
                raw = zf.read(match)
                try:
                    img = Image.open(io.BytesIO(raw))
                    img_rgba = img.convert("RGBA")
                except Exception as e:
                    print(f"  [FAIL] Pillow: {e}", file=sys.stderr)
                    fail += 1
                    found = True
                    break
                if _cook_burnin_ktx2(img_rgba, dst, ktx_tool, max_edge,
                                      dry_run, mission):
                    ok += 1
                else:
                    fail += 1
                found = True
                break

        if not found:
            print(f"  [MISS] {mission}.burnin.tga not found in any zip", file=sys.stderr)
            fail += 1

    return ok, skip, fail


def cook_art(art_zip: Path, art_dir: Path, skip_existing: bool,
             dry_run: bool) -> tuple[int, int, int]:
    """Extract GUI/art files from art zip as-is (TGA/PNG, no KTX2 cook)."""
    ok = skip = fail = 0

    if not art_zip.is_file():
        print(f"  [SKIP] art-zip not found: {art_zip}", file=sys.stderr)
        return 0, 0, 0

    art_dir.mkdir(parents=True, exist_ok=True)

    ART_EXTS = {".tga", ".png", ".fit", ".jpg"}

    with zipfile.ZipFile(art_zip) as zf:
        entries = [n for n in zf.namelist()
                   if not n.endswith("/")
                   and Path(n).suffix.lower() in ART_EXTS]

        print(f"  art zip: {len(entries)} files")

        for name in entries:
            out = art_dir / Path(name).name
            if skip_existing and out.exists():
                skip += 1
                continue
            if dry_run:
                print(f"  [DRY] art/{Path(name).name}")
                ok += 1
                continue
            try:
                data = zf.read(name)
                out.write_bytes(data)
                ok += 1
            except Exception as e:
                print(f"  [FAIL] {name}: {e}", file=sys.stderr)
                fail += 1

    return ok, skip, fail


def write_mod_json(mod_dir: Path, dry_run: bool) -> None:
    dst = mod_dir / "mod.json"
    content = json.dumps(_MOD_JSON_TEMPLATE, indent=2)
    if dry_run:
        print(f"  [DRY] mod.json")
        return
    dst.write_text(content)
    print(f"  [OK] mod.json")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mod-dir",      type=Path, required=True,
                    help="Output mod directory")
    ap.add_argument("--burnin-zips",  type=Path, action="append", default=[],
                    help="Zip(s) containing .burnin.tga files (repeatable)")
    ap.add_argument("--art-zip",      type=Path, default=None,
                    help="Zip containing GUI/art TGA/PNG files")
    ap.add_argument("--missions",     default=_DEFAULT_MISSIONS,
                    help=f"Comma-separated mission stems (default: {_DEFAULT_MISSIONS})")
    ap.add_argument("--max-edge",     type=int, default=4096,
                    help="Cap burnin longest edge in pixels (default: 4096)")
    ap.add_argument("--ktx-tool",     default=_DEFAULT_KTX_TOOL)
    ap.add_argument("--skip-existing", action="store_true")
    ap.add_argument("--dry-run",      action="store_true")
    args = ap.parse_args()

    missions = [m.strip() for m in args.missions.split(",") if m.strip()]
    mod_dir: Path = args.mod_dir.resolve()
    tex_dir = mod_dir / "data" / "textures"
    art_dir = mod_dir / "data" / "art"

    print(f"=== cook_upscaled_assets_mod ===")
    print(f"mod-dir  : {mod_dir}")
    print(f"missions : {missions}")
    print(f"max-edge : {args.max_edge}")
    print(f"dry-run  : {args.dry_run}")

    if not args.dry_run and args.burnin_zips:
        ktx = Path(args.ktx_tool)
        if not ktx.is_file():
            print(f"ERROR: ktx tool not found: {ktx}", file=sys.stderr)
            return 1
        tex_dir.mkdir(parents=True, exist_ok=True)

    t0 = time.time()

    # --- Write mod.json ---
    mod_dir.mkdir(parents=True, exist_ok=True)
    write_mod_json(mod_dir, args.dry_run)

    # --- Burnin colormaps ---
    b_ok = b_skip = b_fail = 0
    if args.burnin_zips:
        print(f"\n--- Burnin colormaps ({len(missions)} missions) ---")
        b_ok, b_skip, b_fail = cook_burnins(
            args.burnin_zips, missions, tex_dir,
            args.max_edge, args.ktx_tool,
            args.skip_existing, args.dry_run,
        )
        print(f"Burnins: {b_ok} cooked, {b_skip} skipped, {b_fail} failed")
    else:
        print("\n[SKIP] No --burnin-zips specified")

    # --- Art/GUI textures ---
    a_ok = a_skip = a_fail = 0
    if args.art_zip:
        print(f"\n--- Art/GUI textures ({args.art_zip.name}) ---")
        a_ok, a_skip, a_fail = cook_art(
            args.art_zip, art_dir,
            args.skip_existing, args.dry_run,
        )
        print(f"Art: {a_ok} extracted, {a_skip} skipped, {a_fail} failed")
    else:
        print("\n[SKIP] No --art-zip specified")

    dt = time.time() - t0
    total_fail = b_fail + a_fail
    print(f"\n=== DONE in {dt:.1f}s  fail={total_fail} ===")
    if not args.dry_run:
        print(f"Mod at: {mod_dir}")

    return 1 if total_fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
