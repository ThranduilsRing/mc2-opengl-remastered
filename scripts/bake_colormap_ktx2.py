#!/usr/bin/env python3
"""scripts/bake_colormap_ktx2.py -- COLORMAP-BC7-KTX2-1 offline cook step.

Converts each mission's .burnin.tga (or .burnin.jpg fallback) to a
.burnin.ktx2 BC7 sidecar that BuildColormapAtlas uploads via
glCompressedTexImage2D(GL_COMPRESSED_RGBA_BPTC_UNORM).

Pipeline per file:
  1. Pillow loads .burnin.tga (handles BGRA->RGBA automatically).
  2. img.convert("RGBA") -> temp PNG (clean RGBA, no channel confusion).
  3. ktx create --encode uastc --format R8G8B8A8_UNORM --assign-tf linear
              --levels 1  <png>  <uastc.ktx2>
  4. ktx transcode --target bc7  <uastc.ktx2>  <out.burnin.ktx2>

Why Pillow for loading: Pillow handles TGA BGRA->RGBA channel swap
automatically; no manual swizzle needed. The .burnin.tga on disk is
stored in BGRA order (MC2 native), and Pillow reads it as RGBA.

Why --format R8G8B8A8_UNORM (not SRGB): the existing RGBA8 path uploads
with GL_RGBA8 (not GL_SRGB8_ALPHA8), so colormap is treated as linear.
BC7 with vkFormat=145 (UNORM) matches.

Why --levels 1 (no mips): colormap is sampled at a fixed world-space
scale in BuildColormapAtlas path; no LOD needed. Keeps sidecar small.

Usage:
  py -3 scripts/bake_colormap_ktx2.py [options]

  --data-dir   DIR   Path to the deployed data/textures directory.
                     Default: A:/Games/mc2-opengl/mc2-win64-v0.3/data/textures
  --ktx-tool   PATH  Path to ktx.exe (KTX-Software CLI).
                     Default: A:/Games/mc2-tools/ktx/ktx.exe
  --pattern    GLOB  Glob pattern for burnin files within data-dir.
                     Default: *.burnin.tga
  --skip-existing    Skip if .burnin.ktx2 already exists.
  --dry-run          Print what would be done, don't run.
  --limit      N     Process at most N files (for testing).
  --mission    NAME  Process only this mission stem (e.g. mc2_01).
  --max-edge   N     Cap longest edge to N pixels before cooking (0=no cap).
                     Uses LANCZOS downscale. Does not upscale.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not found. Install: pip install pillow", file=sys.stderr)
    sys.exit(1)

_DEFAULT_DATA_DIR = r"A:/Games/mc2-opengl/mc2-win64-v0.3/data/textures"
_DEFAULT_KTX_TOOL = r"A:/Games/mc2-tools/ktx/ktx.exe"


def bake_one(src: Path, ktx_tool: str, dry_run: bool, max_edge: int = 0) -> bool:
    """Bake src (.burnin.tga or .burnin.jpg) -> src.with_suffix('.ktx2').
    Returns True on success, False on failure."""
    dst = src.with_suffix(".ktx2")
    if dry_run:
        print(f"[DRY] {src.name} -> {dst.name}")
        return True

    try:
        img = Image.open(src)
    except Exception as e:
        print(f"[SKIP] Pillow failed to open {src.name}: {e}", file=sys.stderr)
        return False

    src_w, src_h = img.size
    img_rgba = img.convert("RGBA")

    if max_edge > 0 and max(src_w, src_h) > max_edge:
        scale = max_edge / max(src_w, src_h)
        new_w = int(src_w * scale)
        new_h = int(src_h * scale)
        img_rgba = img_rgba.resize((new_w, new_h), Image.LANCZOS)
        print(f"  resized {src_w}x{src_h} -> {new_w}x{new_h}")

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        png_path = tdp / "src.png"
        uastc_path = tdp / "uastc.ktx2"

        img_rgba.save(png_path, format="PNG")

        # Step 1: encode to UASTC (intermediate, required by ktx transcode)
        create_cmd = [
            ktx_tool, "create",
            "--encode", "uastc",
            "--format", "R8G8B8A8_UNORM",
            "--assign-tf", "linear",
            "--levels", "1",     # single mip level (no pyramid)
            str(png_path), str(uastc_path),
        ]
        r = subprocess.run(create_cmd, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        if r.returncode != 0:
            print(f"[FAIL] ktx create failed for {src.name}: "
                  f"rc={r.returncode} {(r.stderr or r.stdout).strip()[:200]}",
                  file=sys.stderr)
            return False

        # Step 2: transcode UASTC -> BC7
        transcode_cmd = [
            ktx_tool, "transcode", "--target", "bc7",
            str(uastc_path), str(dst),
        ]
        r = subprocess.run(transcode_cmd, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        if r.returncode != 0:
            print(f"[FAIL] ktx transcode failed for {src.name}: "
                  f"rc={r.returncode} {(r.stderr or r.stdout).strip()[:200]}",
                  file=sys.stderr)
            if dst.exists():
                dst.unlink()
            return False

    dst_bytes = dst.stat().st_size
    print(f"[OK] {src.name} ({src_w}x{src_h}) -> {dst.name} "
          f"src={src.stat().st_size//1024}KB dst={dst_bytes//1024}KB "
          f"ratio={src.stat().st_size/max(dst_bytes,1):.1f}x")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data-dir",      default=_DEFAULT_DATA_DIR)
    ap.add_argument("--ktx-tool",      default=_DEFAULT_KTX_TOOL)
    ap.add_argument("--pattern",       default="*.burnin.tga")
    ap.add_argument("--skip-existing", action="store_true")
    ap.add_argument("--dry-run",       action="store_true")
    ap.add_argument("--limit",         type=int, default=0)
    ap.add_argument("--mission",       action="append", default=[],
                    help="Filter to mission stem (repeatable, e.g. mc2_01)")
    ap.add_argument("--max-edge",      type=int, default=0,
                    help="Cap longest edge to N pixels (0=no cap, LANCZOS downscale)")
    args = ap.parse_args()

    data_dir = Path(args.data_dir)
    if not data_dir.is_dir():
        print(f"ERROR: data-dir not found: {data_dir}", file=sys.stderr)
        return 1

    ktx_tool = args.ktx_tool
    if not args.dry_run and not Path(ktx_tool).is_file():
        print(f"ERROR: ktx tool not found: {ktx_tool}", file=sys.stderr)
        return 1

    files = sorted(data_dir.glob(args.pattern))

    # Filter by --mission if given
    if args.mission:
        stems = set(args.mission)
        files = [f for f in files
                 if any(f.stem.startswith(m) or f.stem == m + ".burnin" for m in stems)
                 or any(f.stem == m + ".burnin" for m in stems)
                 or any(f.name.startswith(m + ".burnin") for m in stems)]

    if args.limit:
        files = files[:args.limit]

    print(f"Found {len(files)} file(s) matching '{args.pattern}' in {data_dir}")
    if not files:
        print("Nothing to do.")
        return 0

    ok = skip = fail = 0
    for src in files:
        dst = src.with_suffix(".ktx2")
        if args.skip_existing and dst.exists():
            skip += 1
            continue
        if bake_one(src, ktx_tool, args.dry_run, args.max_edge):
            ok += 1
        else:
            fail += 1

    print(f"\nDone: {ok} baked, {skip} skipped, {fail} failed.")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
