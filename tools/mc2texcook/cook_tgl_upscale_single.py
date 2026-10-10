#!/usr/bin/env python3
"""cook_tgl_upscale_single.py -- PIL LANCZOS upscale + BC7 KTX2 cook for a single TGL texture.

Used when no AI upscaler (Real-ESRGAN etc.) is available. Upscales a source TGA
using high-quality Lanczos resampling then cooks to BC7 KTX2 via ktx.exe.

When the source is already full-resolution (e.g. extracted from release_assets),
use --no-upscale to cook at native resolution with no resize step.

Phase 2 baseline cook (upscale 128->512):
    py -3 tools/mc2texcook/cook_tgl_upscale_single.py \\
        --src  data/tgl/128/fireantrgb.tga \\
        --dst  data/tgl/128/fireantrgb.ktx2 \\
        --scale 4

Source-quality cook (no upscale, e.g. 512x512 TGA from release_assets):
    py -3 tools/mc2texcook/cook_tgl_upscale_single.py \\
        --src  build/tga_extract/fireantrgb.tga \\
        --dst  build/ktx2_cooked/fireantrgb.ktx2 \\
        --no-upscale

Usage:
    py -3 cook_tgl_upscale_single.py --src SRC.tga --dst OUT.ktx2
        [--scale N]       integer upscale factor (default: 4; 128->512)
        [--target-res N]  explicit output long-edge cap (overrides --scale)
        [--no-upscale]    cook at native source resolution, no resize
        [--ktx-tool PATH] path to ktx.exe (default: A:/Games/mc2-tools/ktx/ktx.exe)
        [--no-mips]       cook single-level (no mip chain)
        [--dry-run]       print plan, do not write output

Stdlib + PIL/Pillow only. No other pip deps.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

KTX_DEFAULT = "A:/Games/mc2-tools/ktx/ktx.exe"


def cook_upscale(src: Path, dst: Path, scale: int, target_res: int,
                 no_upscale: bool,
                 ktx_tool: str, gen_mips: bool, dry_run: bool) -> int:
    from PIL import Image  # type: ignore

    img = Image.open(src).convert("RGBA")
    src_w, src_h = img.size
    print(f"Source: {src_w}x{src_h}  ({src})")

    if no_upscale:
        out_w, out_h = src_w, src_h
        print(f"Output: {out_w}x{out_h}  (method=none, native resolution)")
    elif target_res:
        m = max(src_w, src_h)
        ratio = target_res / m
        out_w = max(1, round(src_w * ratio))
        out_h = max(1, round(src_h * ratio))
        print(f"Upscaled: {out_w}x{out_h}  (method=PIL_LANCZOS, target_res={target_res})")
    else:
        out_w = src_w * scale
        out_h = src_h * scale
        print(f"Upscaled: {out_w}x{out_h}  (method=PIL_LANCZOS, scale={scale}x)")

    print(f"Output: {dst}  (BC7_SRGB, {'with mips' if gen_mips else 'no mips'})")

    if dry_run:
        print("DRY RUN: no files written.")
        return 0

    if no_upscale:
        upscaled = img
    else:
        upscaled = img.resize((out_w, out_h), Image.LANCZOS)
    dst.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        png_path = tdp / "src.png"
        uastc_path = tdp / "uastc.ktx2"
        upscaled.save(png_path, format="PNG")

        create_cmd = [ktx_tool, "create", "--encode", "uastc",
                      "--format", "R8G8B8A8_SRGB", "--assign-tf", "srgb"]
        if gen_mips:
            create_cmd.append("--generate-mipmap")
        create_cmd += [str(png_path), str(uastc_path)]

        transcode_cmd = [ktx_tool, "transcode", "--target", "bc7",
                         str(uastc_path), str(dst)]

        for step_name, cmd in [("uastc encode", create_cmd), ("bc7 transcode", transcode_cmd)]:
            r = subprocess.run(cmd, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
            if r.returncode != 0:
                print(f"ERROR: {step_name} failed (rc={r.returncode}):",
                      file=sys.stderr)
                print((r.stderr or r.stdout).strip()[:400], file=sys.stderr)
                return 1
            print(f"  {step_name}: OK")

    size = dst.stat().st_size
    print(f"Wrote: {dst}  ({size:,} bytes)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="PIL LANCZOS upscale + BC7 KTX2 cook for a single TGL texture.")
    ap.add_argument("--src", type=Path, required=True, help="Source .tga file")
    ap.add_argument("--dst", type=Path, required=True, help="Output .ktx2 path")
    ap.add_argument("--scale", type=int, default=4, metavar="N",
                    help="Integer upscale factor (default: 4)")
    ap.add_argument("--target-res", type=int, default=0, metavar="N",
                    help="Explicit output long-edge cap (overrides --scale)")
    ap.add_argument("--no-upscale", action="store_true",
                    help="Cook at native source resolution, no resize (overrides --scale/--target-res)")
    ap.add_argument("--ktx-tool", default=KTX_DEFAULT, metavar="PATH",
                    help=f"Path to ktx.exe (default: {KTX_DEFAULT})")
    ap.add_argument("--no-mips", action="store_true",
                    help="Cook single-level KTX2 (no mip chain)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print plan, do not write output")
    args = ap.parse_args()

    if not args.src.is_file():
        print(f"ERROR: source not found: {args.src}", file=sys.stderr)
        return 1
    if not args.dry_run and not Path(args.ktx_tool).is_file():
        print(f"ERROR: ktx.exe not found: {args.ktx_tool}", file=sys.stderr)
        return 1

    return cook_upscale(
        src=args.src,
        dst=args.dst,
        scale=args.scale,
        target_res=args.target_res,
        no_upscale=args.no_upscale,
        ktx_tool=args.ktx_tool,
        gen_mips=not args.no_mips,
        dry_run=args.dry_run,
    )


if __name__ == "__main__":
    sys.exit(main())
