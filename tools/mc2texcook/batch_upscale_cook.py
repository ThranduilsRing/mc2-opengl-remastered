#!/usr/bin/env python3
"""
batch_upscale_cook.py -- Batch-cook pre-upscaled TGAs to KTX2 BC7.

Iterates a directory of TGA files that have already been upscaled externally
(e.g. via Real-ESRGAN or any AI upscaler) and cooks each one to KTX2 BC7
using the ktx CLI (same two-step uastc->bc7 pipeline as cook_tgl_tiers.py).

Does NOT perform any upscaling itself -- input TGAs are assumed to be
at final resolution. For upscaling, use a dedicated tool first.

Usage:
    py -3 tools/mc2texcook/batch_upscale_cook.py \\
        --input-dir build/tga_extract/ \\
        --output-dir build/ktx2_cooked/ \\
        [--force] \\
        [--preset albedo] \\
        [--ktx-tool PATH]

Exit codes:
    0   all files succeeded (or were skipped)
    1   one or more files failed

Stdlib only: no pip dependencies (uses subprocess for ktx CLI, tempfile, pathlib).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path


_DEFAULT_KTX_TOOL = r"A:/Games/mc2-tools/ktx/ktx.exe"

_KNOWN_PRESETS = {'albedo', 'normal', 'orm', 'emissive', 'mask'}

# Preset -> (vk_format, assign_tf) for the ktx create step
_PRESET_PARAMS = {
    'albedo':   ('R8G8B8A8_SRGB',  'srgb'),
    'emissive': ('R8G8B8A8_SRGB',  'srgb'),
    'normal':   ('R8G8B8A8_UNORM', 'linear'),
    'orm':      ('R8G8B8A8_UNORM', 'linear'),
    'mask':     ('R8G8B8A8_UNORM', 'linear'),
}


def cook_tga_to_ktx2(src: Path, dst: Path, ktx_tool: str, preset: str = 'albedo') -> None:
    """Cook one TGA -> KTX2 BC7 using the two-step uastc->bc7 pipeline.

    Two steps:
      1. ktx create --encode uastc --format <FMT> --assign-tf <TF>
                    --generate-mipmap <src.tga> <tmp_uastc.ktx2>
      2. ktx transcode --target bc7 <tmp_uastc.ktx2> <dst.ktx2>

    Raises RuntimeError on any subprocess failure.
    """
    fmt, tf = _PRESET_PARAMS[preset]

    dst.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        uastc_path = Path(td) / 'uastc.ktx2'

        create_cmd = [
            ktx_tool, 'create',
            '--encode', 'uastc',
            '--format', fmt,
            '--assign-tf', tf,
            '--generate-mipmap',
            str(src),
            str(uastc_path),
        ]
        transcode_cmd = [
            ktx_tool, 'transcode',
            '--target', 'bc7',
            str(uastc_path),
            str(dst),
        ]

        for cmd in (create_cmd, transcode_cmd):
            r = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if r.returncode != 0:
                step = cmd[1]
                detail = (r.stderr or r.stdout).strip()[:400]
                raise RuntimeError(
                    f"ktx {step} failed (rc={r.returncode}): {detail}"
                )


def main() -> int:
    ap = argparse.ArgumentParser(
        description='Batch-cook pre-upscaled TGAs to KTX2 BC7 using the ktx CLI.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument(
        '--input-dir', type=Path, required=True,
        help='Directory containing upscaled TGA files to cook',
    )
    ap.add_argument(
        '--output-dir', type=Path, required=True,
        help='Output directory for KTX2 files (mirrors source directory structure)',
    )
    ap.add_argument(
        '--force', action='store_true',
        help='Re-cook even if a .ktx2 output already exists (default: skip existing)',
    )
    ap.add_argument(
        '--preset', default='albedo', choices=sorted(_KNOWN_PRESETS),
        help='Texture usage preset controlling color space (default: albedo)',
    )
    ap.add_argument(
        '--recursive', action='store_true',
        help='Recurse into subdirectories (preserves relative structure in --output-dir)',
    )
    ap.add_argument(
        '--ktx-tool', default=_DEFAULT_KTX_TOOL,
        help=f'Path to ktx CLI executable (default: {_DEFAULT_KTX_TOOL})',
    )
    args = ap.parse_args()

    ktx_tool: str = args.ktx_tool
    if not Path(ktx_tool).is_file():
        print(f'ERROR: ktx CLI not found: {ktx_tool}', file=sys.stderr)
        print('       Install KTX-Software and pass --ktx-tool <path>', file=sys.stderr)
        return 1

    input_dir: Path = args.input_dir.resolve()
    output_dir: Path = args.output_dir.resolve()

    if not input_dir.is_dir():
        print(f'ERROR: --input-dir is not a directory: {input_dir}', file=sys.stderr)
        return 1

    # Collect TGA files
    if args.recursive:
        candidates = sorted(
            p for p in input_dir.rglob('*')
            if p.is_file() and p.suffix.lower() == '.tga'
        )
    else:
        candidates = sorted(
            p for p in input_dir.iterdir()
            if p.is_file() and p.suffix.lower() == '.tga'
        )

    total = len(candidates)
    if total == 0:
        print(f'No .tga files found in {input_dir}')
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f'batch_upscale_cook: input_dir={input_dir}')
    print(f'                    output_dir={output_dir}')
    print(f'                    preset={args.preset}  force={args.force}  total_tga={total}')

    cooked = 0
    skipped = 0
    failed = 0
    failures: list[dict] = []

    t0 = time.time()
    for src in candidates:
        rel = src.relative_to(input_dir)
        dst = (output_dir / rel).with_suffix('.ktx2')

        if not args.force and dst.exists():
            skipped += 1
            continue

        try:
            cook_tga_to_ktx2(src, dst, ktx_tool, preset=args.preset)
            cooked += 1
            print(f'  OK  {rel} -> {dst.name}')
        except Exception as exc:  # noqa: BLE001
            failed += 1
            failures.append({'file': str(src), 'error': str(exc)})
            print(f'  FAIL {rel}: {exc}', file=sys.stderr)

    dt = time.time() - t0
    print()
    print(f'DONE in {dt:.1f}s  total={total}  cooked={cooked}  skipped={skipped}  failed={failed}')

    if failures:
        print(f'Failures ({len(failures)}):')
        for f in failures:
            print(f"  {f['file']}: {f['error']}")

    return 1 if failed > 0 else 0


if __name__ == '__main__':
    sys.exit(main())
