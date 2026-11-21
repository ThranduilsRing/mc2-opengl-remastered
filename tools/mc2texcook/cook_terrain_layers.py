#!/usr/bin/env python3
"""
cook_terrain_layers.py -- Batch-cook terrain material-lib layer textures
(TERRAIN-MATERIAL-TEXTURE-REMAP-1 prep) from terrain_layer_manifest.json.

Reads terrain_layer_manifest.json (channel -> source pack -> source map paths,
either loose files or members inside a .zip download), extracts/normalizes
each source (PNG/JPG/EXR all handled via cv2, see cook_pbr_maps.normalize_to_png),
and cooks two BC7 KTX2 outputs per channel:

  <channel>_albedo.ktx2  -- BC7 sRGB   (from the pack's albedo/diff/color map)
  <channel>_normal.ktx2  -- BC7 linear (rgb = tangent normal, a = height/displacement,
                             matching the engine's matNormalArray .rgb/.a convention)

roughness/ao are NOT cooked in this pass (scalars-v2 ruling -- see manifest
"_comment" and data/terrain_materials.json *_roughness/*_ao keys).

Output: <output_dir>/<channel>_<map>.ktx2, 2048x2048, per manifest["output_size"]
and manifest["output_dir"] (defaults applied if absent).

Usage:
    py -3 tools/mc2texcook/cook_terrain_layers.py \\
        --manifest tools/mc2texcook/terrain_layer_manifest.json \\
        [--out-dir data/terrain_layers] [--channel rock] [--force] [--dry-run]

External tools required:
    ktx.exe  -- A:\\Games\\mc2-tools\\ktx\\ktx.exe  (default, override with --ktx-exe)

Deps: stdlib + Pillow + numpy + opencv-python(-headless). All already present
in this repo's Python environment (see cook_bc6h_hdri.py / mc2texcook.py for
precedent).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import cook_pbr_maps as pbr  # noqa: E402

# The engine detail-normal layers terrain_tier1.frag samples (MAT_LAYER_ROCK .. MAT_LAYER_MARBLE_CLIFF). Each
# data/textures/matN_normal.tga gets a matN_normal.ktx2 sidecar next to it (BC7 UNORM, box mip chain), which the
# terrain loader prefers under the usual sidecar precedence (a mod TGA beats a base KTX).
ENGINE_NORMAL_SLOTS = range(6)


def engine_normal_texels(tga_path):
    """The texels the terrain samples for a matN_normal.tga: RGBA = (normal x, y, z, height), top row first, A=255 for 24-bit."""
    import numpy as np
    from PIL import Image
    with Image.open(tga_path) as img:
        return np.ascontiguousarray(np.asarray(img.convert('RGBA')))


def box_mips(texels):
    """The full mip chain the engine's TGA import builds (mc2tex::boxDownsampleRgba8): per channel (a+b+c+d+2)/4 on 2x2
    blocks, no renormalisation. The terrain textures are square powers of two. Returns the uint8 HxWx4 levels, largest first."""
    import numpy as np
    levels = [texels]
    while levels[-1].shape[0] > 1:
        t = levels[-1].astype(np.uint32)
        levels.append(((t[0::2, 0::2] + t[1::2, 0::2] + t[0::2, 1::2] + t[1::2, 1::2] + 2) // 4).astype(np.uint8))
    return levels


def write_bc7_ktx2(path, levels):
    """BC7 UNORM (linear) KTX2 holding the given RGBA8 mip levels (largest first), encoded per level by texcook (alpha kept)."""
    sys.path.insert(0, str(_HERE.parents[1]))
    from tools.content_pipeline import texcook
    h, w = levels[0].shape[:2]
    vk_format, blocks = texcook.encode_levels(levels, 'orm')
    with open(path, 'wb') as f:
        f.write(texcook.write_ktx2(vk_format, w, h, blocks))


def cook_engine_normals(src_dir, out_dir=None, ktx_exe=None, force=False):
    """matN_normal.tga -> matN_normal.ktx2 for the sampled slots: BC7 UNORM, every level encoded from the mip chain the TGA
    import builds (alpha = height kept). An output newer than its source is kept.
    Returns [(ktx2 path, ok)] per slot."""
    out_dir = out_dir or src_dir
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for slot in ENGINE_NORMAL_SLOTS:
        src = os.path.join(src_dir, f'mat{slot}_normal.tga')
        dst = os.path.join(out_dir, f'mat{slot}_normal.ktx2')
        if not os.path.isfile(src):
            results.append((dst, False))
            continue
        if not force and os.path.isfile(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            results.append((dst, True))
            continue
        staged = dst + '.tmp'
        write_bc7_ktx2(staged, box_mips(engine_normal_texels(src)))
        os.replace(staged, dst)
        results.append((dst, True))
    return results


DEFAULT_MANIFEST = _HERE / 'terrain_layer_manifest.json'
DEFAULT_OUTPUT_SIZE = 2048
DEFAULT_OUTPUT_DIR = 'data/terrain_layers'


def load_manifest(path):
    with open(path, 'r', encoding='utf-8') as f:
        return json.load(f)


def validate_manifest(manifest):
    """
    Structural validation. Returns a list of error strings (empty = valid).
    Kept as a standalone function so pytest can exercise it directly.
    """
    errors = []

    if not isinstance(manifest, dict):
        return ['manifest root must be an object']

    if 'layers' not in manifest or not isinstance(manifest['layers'], dict):
        errors.append('manifest missing "layers" object')
        return errors

    if not manifest['layers']:
        errors.append('manifest "layers" is empty')

    size = manifest.get('output_size', DEFAULT_OUTPUT_SIZE)
    if not isinstance(size, int) or size <= 0 or (size & (size - 1)) != 0:
        errors.append(f'output_size must be a positive power-of-two int, got {size!r}')

    for channel, entry in manifest['layers'].items():
        if not isinstance(entry, dict):
            errors.append(f'layer "{channel}": entry must be an object')
            continue

        for required_key in ('source_pack', 'source_dir', 'maps', 'license'):
            if required_key not in entry:
                errors.append(f'layer "{channel}": missing required key "{required_key}"')

        maps = entry.get('maps')
        if not isinstance(maps, dict):
            errors.append(f'layer "{channel}": "maps" must be an object')
            continue

        if not maps.get('albedo'):
            errors.append(f'layer "{channel}": "maps.albedo" is required (no albedo source)')
        if not maps.get('normal'):
            errors.append(f'layer "{channel}": "maps.normal" is required (no normal source)')

        # height is optional (cliff has none) but if present must be a string.
        height = maps.get('height')
        if height is not None and not isinstance(height, str):
            errors.append(f'layer "{channel}": "maps.height" must be a string or null')

        source_dir = entry.get('source_dir', '')
        if source_dir and not os.path.exists(source_dir):
            errors.append(f'layer "{channel}": source_dir does not exist on disk: {source_dir}')

    return errors


def _extract_member(zip_path, member, dest_dir):
    """Extract a single member from a zip to dest_dir, return the extracted path."""
    with zipfile.ZipFile(zip_path, 'r') as zf:
        zf.extract(member, dest_dir)
    return os.path.join(dest_dir, member)


def _resolve_source(source_dir, map_rel_path, tmpdir):
    """
    Resolve a manifest map path to a real file on disk.

    source_dir may be a .zip (map_rel_path is extracted as a member) or a
    loose directory (map_rel_path is joined directly).
    """
    if source_dir.lower().endswith('.zip'):
        return _extract_member(source_dir, map_rel_path.replace('\\', '/'), tmpdir)
    return os.path.join(source_dir, map_rel_path)


def cook_channel(channel, entry, out_dir, ktx_exe, output_size, force, dry_run):
    """
    Cook one channel's albedo + normal(+height-alpha) KTX2 outputs.
    Returns a dict: {'albedo': (path, ok, size_bytes), 'normal': (...)}.
    """
    maps = entry['maps']
    source_dir = entry['source_dir']
    results = {}

    albedo_out = os.path.join(out_dir, f'{channel}_albedo.ktx2')
    normal_out = os.path.join(out_dir, f'{channel}_normal.ktx2')

    if dry_run:
        print(f'[{channel}] DRY-RUN would cook:')
        print(f'    albedo <- {maps["albedo"]}  -> {albedo_out}')
        print(f'    normal <- {maps["normal"]} (+height {maps.get("height")}) -> {normal_out}')
        return {
            'albedo': (albedo_out, True, 0),
            'normal': (normal_out, True, 0),
        }

    os.makedirs(out_dir, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=f'cook_terrain_{channel}_') as tmpdir:
        # --- albedo -----------------------------------------------------
        if not force and os.path.exists(albedo_out):
            print(f'[{channel}] albedo SKIP (exists, use --force)')
            results['albedo'] = (albedo_out, True, os.path.getsize(albedo_out))
        else:
            albedo_src = _resolve_source(source_dir, maps['albedo'], tmpdir)
            albedo_png = os.path.join(tmpdir, f'{channel}_albedo_norm.png')
            pbr.normalize_to_png(albedo_src, albedo_png, srgb=True)
            pbr.resize_to_square(albedo_png, albedo_png, output_size)

            print(f'[{channel}] cooking albedo ({os.path.basename(albedo_src)}) -> {albedo_out}')
            # mips=True is load-bearing. The engine
            # albedo array samples these tiled at terrain distances; a single-level
            # 2048 sq. layer shimmers/aliases badly. A cook that omits
            # --generate-mipmap is rejected by the engine loader (mipCount > 1).
            ok = pbr.cook_source_to_bc7(
                ktx_exe, albedo_png, albedo_out,
                vk_format='R8G8B8A8_SRGB', oetf='srgb', tmpdir=tmpdir, mips=True,
            )
            size = os.path.getsize(albedo_out) if os.path.exists(albedo_out) else 0
            results['albedo'] = (albedo_out, ok, size)

        # --- normal (+height packed into alpha) --------------------------
        if not force and os.path.exists(normal_out):
            print(f'[{channel}] normal SKIP (exists, use --force)')
            results['normal'] = (normal_out, True, os.path.getsize(normal_out))
        else:
            normal_src = _resolve_source(source_dir, maps['normal'], tmpdir)
            normal_norm_png = os.path.join(tmpdir, f'{channel}_normal_norm.png')
            pbr.normalize_to_png(normal_src, normal_norm_png, srgb=False)

            height_src = None
            if maps.get('height'):
                height_src = _resolve_source(source_dir, maps['height'], tmpdir)
                height_norm_png = os.path.join(tmpdir, f'{channel}_height_norm.png')
                pbr.normalize_to_png(height_src, height_norm_png, srgb=False)
                height_src = height_norm_png

            packed_png = os.path.join(tmpdir, f'{channel}_normal_packed.png')
            pbr.pack_height_into_normal_alpha(normal_norm_png, height_src, packed_png, output_size)

            print(f'[{channel}] cooking normal(+height alpha) -> {normal_out}')
            # mips=True: keep normal(+height) cooks mip-complete like albedo so a
            # future normal-source remap does not re-hit the shimmer bug.
            ok = pbr.cook_source_to_bc7(
                ktx_exe, packed_png, normal_out,
                vk_format='R8G8B8A8_UNORM', oetf='linear', tmpdir=tmpdir, mips=True,
            )
            size = os.path.getsize(normal_out) if os.path.exists(normal_out) else 0
            results['normal'] = (normal_out, ok, size)

    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', default=str(DEFAULT_MANIFEST),
                        help=f'Path to terrain_layer_manifest.json (default: {DEFAULT_MANIFEST})')
    parser.add_argument('--out-dir', default=None,
                        help=f'Output dir override (default: manifest["output_dir"] or {DEFAULT_OUTPUT_DIR})')
    parser.add_argument('--channel', action='append', default=None,
                        help='Cook only this channel (repeatable). Default: all channels in manifest.')
    parser.add_argument('--force', action='store_true', help='Re-cook even if output exists.')
    parser.add_argument('--dry-run', action='store_true', help='Print planned actions, cook nothing.')
    parser.add_argument('--ktx-exe', default=pbr.DEFAULT_KTX_EXE,
                        help=f'Path to ktx.exe (default: {pbr.DEFAULT_KTX_EXE})')
    parser.add_argument('--engine-normals', metavar='TEXTURES_DIR', default=None,
                        help='Cook the sampled matN_normal.tga layers in TEXTURES_DIR to lossless RGBA8 mip-complete matN_normal.ktx2 '
                             'sidecars (into --out-dir, default TEXTURES_DIR) and exit.')
    args = parser.parse_args()

    if args.engine_normals:
        results = cook_engine_normals(args.engine_normals, args.out_dir, args.ktx_exe, args.force)
        for path, ok in results:
            print(f'  {"OK" if ok else "FAILED"}  {path}')
        sys.exit(0 if all(ok for _, ok in results) else 1)

    manifest = load_manifest(args.manifest)
    errors = validate_manifest(manifest)
    if errors:
        print('ERROR: manifest failed validation:', file=sys.stderr)
        for e in errors:
            print(f'  - {e}', file=sys.stderr)
        sys.exit(1)

    if not args.dry_run and not os.path.exists(args.ktx_exe):
        print(f'ERROR: ktx.exe not found at {args.ktx_exe}', file=sys.stderr)
        sys.exit(1)

    output_size = manifest.get('output_size', DEFAULT_OUTPUT_SIZE)
    out_dir = args.out_dir or manifest.get('output_dir', DEFAULT_OUTPUT_DIR)
    out_dir = os.path.abspath(out_dir)

    layers = manifest['layers']
    channels = args.channel if args.channel else list(layers.keys())

    unknown = [c for c in channels if c not in layers]
    if unknown:
        print(f'ERROR: unknown channel(s): {", ".join(unknown)}', file=sys.stderr)
        print(f'Known channels: {", ".join(layers.keys())}', file=sys.stderr)
        sys.exit(1)

    print(f'Manifest: {args.manifest}')
    print(f'Output dir: {out_dir}  (size {output_size}x{output_size})')
    print(f'Channels: {", ".join(channels)}')
    print()

    all_results = {}
    had_error = False
    for channel in channels:
        entry = layers[channel]
        res = cook_channel(channel, entry, out_dir, args.ktx_exe, output_size, args.force, args.dry_run)
        all_results[channel] = res
        for label, (path, ok, size) in res.items():
            if not ok:
                had_error = True

    print('\n' + '=' * 70)
    print('Cook summary:')
    for channel, res in all_results.items():
        for label, (path, ok, size) in res.items():
            status = 'OK' if ok else 'FAILED'
            print(f'  {channel:12s} {label:8s}: {size:>12,} bytes  [{status}]  {path}')

    if had_error:
        print('\nERRORS occurred during cook.', file=sys.stderr)
        sys.exit(1)
    print('\nAll channels cooked successfully.')


if __name__ == '__main__':
    main()
