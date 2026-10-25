#!/usr/bin/env python3
"""tools/mc2texcook/batch_cook.py -- TRACKG asset pipeline: batch KTX2 cook wrapper.

Part of the TRACKG asset pipeline.  Offline, in-process bulk wrapper around
mc2texcook.py: walks a source tree of texture art (TGA/PNG/EXR/TXM) and cooks
each file to a KTX2 RGBA8 sidecar (full mip chain) using the same presets as the
single-file tool (albedo=sRGB/43, normal/orm/mask/emissive=UNORM/37).

Outputs are LOOSE .ktx2 sidecars -- generated build artifacts that live in the
deploy tree, NOT committed to git.  This wrapper itself is reusable for any
src->dst cook batch.

Usage:
    py -3 batch_cook.py --src DIR --dst DIR [--preset albedo] [--ext .tga]
        [--recursive] [--skip-existing] [--limit N] [--jobs N] [--manifest out.json]

IMPORTANT (Windows): use native Windows paths (A:/Games/...), not MSYS /a/...
paths -- the latter are not understood by Python on Windows.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

# Default portable KTX-Software CLI (KTX-Software 4.4.2) for the --bc7 path.
_DEFAULT_KTX_TOOL = r"A:/Games/mc2-tools/ktx/ktx.exe"

# --- Wire in the single-file cook tool (sibling module) ---------------------
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
import mc2texcook  # noqa: E402


def _cook_one(src_path: Path, dst_path: Path, preset: str, txm_size: int) -> tuple[int, int]:
    """Cook a single file in-process. Returns (src_w, src_h) of the loaded image."""
    img = mc2texcook._load_image(src_path, txm_size=txm_size)
    src_w, src_h = img.size
    rgba, vk_format, dfd = mc2texcook._apply_preset(img, preset)
    mips = mc2texcook._generate_mips(rgba)
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    mc2texcook.write_ktx2(dst_path, mips, vk_format, dfd)
    return src_w, src_h


def _cook_one_bc7(src_path: Path, dst_path: Path, ktx_tool: str,
                  txm_size: int, gen_mips: bool = True,
                  preset: str = "albedo") -> tuple[int, int]:
    """Cook a single file to stored BC7 KTX2 via the KTX-Software CLI.

    Two-step pipeline (produces supercompression=0, BC7 mip chain):
      1. ktx create --encode uastc --format <FMT> --assign-tf <TF>
                    [--generate-mipmap]  <png>  <tmp_uastc.ktx2>
      2. ktx transcode --target bc7  <tmp_uastc.ktx2>  <out.ktx2>

    Slot-aware color space: normal/orm/mask carry LINEAR data and must be cooked
    R8G8B8A8_UNORM (-> BC7_UNORM); albedo/emissive stay R8G8B8A8_SRGB (-> BC7_SRGB).

    Source is decoded by Pillow (handles .tga/.txm via mc2texcook) to a temp PNG
    so the CLI always sees a plain RGBA PNG. Returns (src_w, src_h).

    gen_mips=False (--no-mips) cooks a SINGLE-LEVEL BC7 image: the texmgr
    data/textures path (MC2_TEXMGR_COMPRESSED_UPLOAD) loads these with
    gosHint_DisableMipmap and uploads MIP 0 only, so a mip chain is wasted.
    The static-prop array path keeps mips (default), so leave gen_mips=True there.
    """
    img = mc2texcook._load_image(src_path, txm_size=txm_size)
    src_w, src_h = img.size
    dst_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        png_path = tdp / "src.png"
        uastc_path = tdp / "uastc.ktx2"
        # Force RGBA so BC7 always has an alpha channel (matches the RGBA8 path).
        img.convert("RGBA").save(png_path, format="PNG")

        linear = preset in ("normal", "orm", "mask")
        create_fmt = "R8G8B8A8_UNORM" if linear else "R8G8B8A8_SRGB"
        assign_tf = "linear" if linear else "srgb"
        create_step = [ktx_tool, "create", "--encode", "uastc",
                       "--format", create_fmt, "--assign-tf", assign_tf]
        if gen_mips:
            create_step.append("--generate-mipmap")
        create_step += [str(png_path), str(uastc_path)]

        for step in (
            create_step,
            [ktx_tool, "transcode", "--target", "bc7",
             str(uastc_path), str(dst_path)],
        ):
            r = subprocess.run(step, capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(
                    f"ktx step failed ({step[1]}): rc={r.returncode} "
                    f"{(r.stderr or r.stdout).strip()[:300]}")

    return src_w, src_h


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Batch-cook a tree of source textures to KTX2 sidecars (in-process).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--src", type=Path, required=True, help="Source directory to walk")
    ap.add_argument("--dst", type=Path, required=True, help="Output directory for .ktx2")
    ap.add_argument("--preset", default="albedo",
                    choices=["albedo", "normal", "orm", "emissive", "mask"],
                    help="Texture usage preset (default: albedo)")
    ap.add_argument("--ext", default=".tga", help="Source file extension to match (default: .tga)")
    ap.add_argument("--recursive", action="store_true",
                    help="Recurse into subdirectories (preserve relative structure in --dst)")
    ap.add_argument("--skip-existing", action="store_true",
                    help="Skip files whose .ktx2 output already exists")
    ap.add_argument("--limit", type=int, default=0, metavar="N",
                    help="Cap number of files processed (0 = all)")
    ap.add_argument("--jobs", type=int, default=1, metavar="N",
                    help="Worker count (default 1; >1 reserved, currently serial)")
    ap.add_argument("--size", type=int, default=0, metavar="N",
                    help="For .txm input: resize decoded 64x64 to NxN before cooking (0=native)")
    ap.add_argument("--manifest", type=Path, default=None,
                    help="Optional JSON summary output path")
    ap.add_argument("--bc7", action="store_true",
                    help="Cook to stored BC7 KTX2 via the KTX-Software CLI "
                         "(2-step uastc encode -> bc7 transcode) instead of "
                         "uncompressed RGBA8. albedo/sRGB pipeline.")
    ap.add_argument("--no-mips", action="store_true",
                    help="With --bc7: omit --generate-mipmap so the BC7 cook is "
                         "SINGLE-LEVEL (mip 0 only). Use for texmgr data/textures "
                         "sidecars (MC2_TEXMGR_COMPRESSED_UPLOAD) which load with "
                         "DisableMipmap. Default --bc7 keeps mips (static-prop set).")
    ap.add_argument("--ktx-tool", default=_DEFAULT_KTX_TOOL,
                    help=f"Path to the KTX-Software ktx CLI (default: {_DEFAULT_KTX_TOOL}). "
                         "Only used with --bc7.")
    args = ap.parse_args()

    if args.bc7 and not Path(args.ktx_tool).is_file():
        print(f"ERROR: --bc7 requires the ktx CLI; not found: {args.ktx_tool}",
              file=sys.stderr)
        return 1

    src_dir: Path = args.src.resolve()
    dst_dir: Path = args.dst.resolve()
    ext = args.ext if args.ext.startswith(".") else "." + args.ext
    ext_lc = ext.lower()

    if not src_dir.is_dir():
        print(f"ERROR: --src is not a directory: {src_dir}", file=sys.stderr)
        return 1

    # --- Deterministic file list ---
    if args.recursive:
        candidates = sorted(p for p in src_dir.rglob("*")
                            if p.is_file() and p.suffix.lower() == ext_lc)
    else:
        candidates = sorted(p for p in src_dir.iterdir()
                            if p.is_file() and p.suffix.lower() == ext_lc)

    if args.limit and args.limit > 0:
        candidates = candidates[:args.limit]

    total = len(candidates)
    dst_dir.mkdir(parents=True, exist_ok=True)

    print(f"batch_cook: src={src_dir}")
    print(f"            dst={dst_dir}")
    print(f"            preset={args.preset} ext={ext} recursive={args.recursive} "
          f"skip_existing={args.skip_existing} total_matched={total} "
          f"mode={'bc7' if args.bc7 else 'rgba8'}")

    cooked = 0
    skipped = 0
    failed = 0
    failures: list[dict] = []
    dim_hist: dict[str, int] = {}

    t0 = time.time()
    for i, src_path in enumerate(candidates, 1):
        rel = src_path.relative_to(src_dir)
        dst_path = (dst_dir / rel).with_suffix(".ktx2")

        if args.skip_existing and dst_path.exists():
            skipped += 1
        else:
            try:
                if args.bc7:
                    w, h = _cook_one_bc7(src_path, dst_path, args.ktx_tool, args.size,
                                         gen_mips=not args.no_mips, preset=args.preset)
                else:
                    w, h = _cook_one(src_path, dst_path, args.preset, args.size)
                key = f"{w}x{h}"
                dim_hist[key] = dim_hist.get(key, 0) + 1
                cooked += 1
            except Exception as e:  # noqa: BLE001 -- one bad file must not abort the batch
                failed += 1
                failures.append({"file": str(src_path), "error": f"{type(e).__name__}: {e}"})
                traceback.print_exc(file=sys.stderr)

        if i % 100 == 0 or i == total:
            print(f"  [{i}/{total}] cooked={cooked} skipped={skipped} failed={failed}")

    dt = time.time() - t0
    print(f"\nDONE in {dt:.1f}s  total={total} cooked={cooked} "
          f"skipped={skipped} failed={failed}")
    if dim_hist:
        print("dimHistogram (source dims):")
        for k in sorted(dim_hist, key=lambda s: dim_hist[s], reverse=True):
            print(f"    {k}: {dim_hist[k]}")
    if failures:
        print(f"first failures (up to 5):")
        for f in failures[:5]:
            print(f"    {f['file']}: {f['error']}")

    if args.manifest:
        manifest = {
            "src": str(src_dir),
            "dst": str(dst_dir),
            "preset": args.preset,
            "total": total,
            "cooked": cooked,
            "skipped": skipped,
            "failed": failed,
            "failures": failures,
            "dimHistogram": dim_hist,
        }
        mpath: Path = args.manifest.resolve()
        mpath.parent.mkdir(parents=True, exist_ok=True)
        mpath.write_text(json.dumps(manifest, indent=2))
        print(f"manifest -> {mpath}")

    return 1 if failed and cooked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
