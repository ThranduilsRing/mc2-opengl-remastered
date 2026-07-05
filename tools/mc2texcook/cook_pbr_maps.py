#!/usr/bin/env python3
"""
cook_pbr_maps.py -- Cook an AmbientCG-format PBR material pack to BC7 KTX2.

Outputs:
  {slug}_albedo.ktx2  -- BC7 sRGB  (from _Color.png)
  {slug}_normal.ktx2  -- BC7 linear (from _NormalGL.png)
  {slug}_orm.ktx2     -- BC7 linear (ORM packed: R=AO, G=Roughness, B=Metalness)

Two-step pipeline (matches cook_bc6h_hdri.py pattern):
  PNG  ->  ktx create --encode uastc  ->  uastc.ktx2
       ->  ktx transcode --target bc7 ->  bc7.ktx2

Usage:
    py -3 tools/mc2texcook/cook_pbr_maps.py \\
        --input-dir DIR --out-dir DIR --name SLUG [--force]

External tools required:
    ktx.exe  -- A:\\Games\\mc2-tools\\ktx\\ktx.exe  (default)

Stdlib + Pillow only for the original AmbientCG single-pack CLI below.
The TERRAIN-MATERIAL-TEXTURE-REMAP-1-prep additions further down (normalize_to_png,
pack_height_into_normal_alpha, cook_source_to_bc7) additionally use cv2/numpy to
ingest PolyHaven-style EXR/JPG source maps -- see cook_terrain_layers.py, the
batch driver that calls them.
"""

import argparse
import os
import sys
import subprocess
import tempfile

DEFAULT_KTX_EXE = r'A:\Games\mc2-tools\ktx\ktx.exe'


# ---------------------------------------------------------------------------
# Map detection
# ---------------------------------------------------------------------------

def _suffix_match(name_lower, suffixes):
    return any(name_lower.endswith(s.lower()) for s in suffixes)


def detect_maps(input_dir):
    """
    Scan input_dir for AmbientCG-style PNG files by suffix.
    Returns a dict with keys: albedo, normal, roughness, metalness, ao.
    Values are absolute file paths or None if not found.
    """
    maps = {
        'albedo':    None,
        'normal':    None,
        'roughness': None,
        'metalness': None,
        'ao':        None,
    }
    albedo_suffixes    = ('_color.png', '_colour.png')
    normal_suffixes    = ('_normalgl.png',)
    roughness_suffixes = ('_roughness.png',)
    metalness_suffixes = ('_metalness.png',)
    ao_suffixes        = ('_ambientocclusion.png', '_ao.png')

    for fname in os.listdir(input_dir):
        fl = fname.lower()
        full = os.path.join(input_dir, fname)
        if not fl.endswith('.png'):
            continue
        if _suffix_match(fl, albedo_suffixes):
            maps['albedo'] = full
        elif _suffix_match(fl, normal_suffixes):
            maps['normal'] = full
        elif _suffix_match(fl, roughness_suffixes):
            maps['roughness'] = full
        elif _suffix_match(fl, metalness_suffixes):
            maps['metalness'] = full
        elif _suffix_match(fl, ao_suffixes):
            maps['ao'] = full

    return maps


# ---------------------------------------------------------------------------
# KTX2 cook helpers
# ---------------------------------------------------------------------------

def _ktx_create_uastc(ktx_exe, src_png, uastc_ktx2, vk_format, oetf, mips=False):
    """
    Step 1: PNG -> UASTC KTX2.

    vk_format: e.g. 'R8G8B8A8_SRGB' or 'R8G8B8A8_UNORM'
    oetf:      'srgb' or 'linear'
    mips:      True = bake a full mip chain (--generate-mipmap). Default False
               preserves the historical single-level output for existing
               callers (TERRAIN-MATERIAL-TEXTURES-1: terrain layer arrays need
               mips or tiling terrain albedo shimmers at distance).
    """
    cmd = [
        ktx_exe, 'create',
        '--encode', 'uastc',
        '--format', vk_format,
        '--assign-tf', oetf,
    ]
    if mips:
        cmd.append('--generate-mipmap')
    cmd += [
        src_png,
        uastc_ktx2,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f'  ktx create failed (rc={result.returncode})', file=sys.stderr)
        if result.stderr:
            print(f'  stderr: {result.stderr.strip()}', file=sys.stderr)
        return False
    return True


def _ktx_transcode_bc7(ktx_exe, uastc_ktx2, out_ktx2):
    """Step 2: UASTC KTX2 -> BC7 KTX2."""
    cmd = [
        ktx_exe, 'transcode',
        '--target', 'bc7',
        uastc_ktx2,
        out_ktx2,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f'  ktx transcode failed (rc={result.returncode})', file=sys.stderr)
        if result.stderr:
            print(f'  stderr: {result.stderr.strip()}', file=sys.stderr)
        return False
    return True


def cook_png_to_bc7(ktx_exe, src_png, out_ktx2, vk_format, oetf, tmpdir, mips=False):
    """
    Cook src_png -> BC7 KTX2 at out_ktx2.
    Uses tmpdir for the intermediate UASTC file.
    mips=True bakes a full mip chain (see _ktx_create_uastc).
    Returns True on success.
    """
    basename = os.path.splitext(os.path.basename(out_ktx2))[0]
    uastc_tmp = os.path.join(tmpdir, f'{basename}_uastc.ktx2')

    print(f'  [1/2] ktx create uastc ({vk_format}, {oetf}, mips={mips}) ...')
    if not _ktx_create_uastc(ktx_exe, src_png, uastc_tmp, vk_format, oetf, mips=mips):
        return False

    print(f'  [2/2] ktx transcode bc7 ...')
    if not _ktx_transcode_bc7(ktx_exe, uastc_tmp, out_ktx2):
        return False

    # Clean up intermediate
    try:
        os.remove(uastc_tmp)
    except OSError:
        pass

    return True


# ---------------------------------------------------------------------------
# ORM packing
# ---------------------------------------------------------------------------

def pack_orm(ao_path, roughness_path, metalness_path, out_png):
    """
    Pack AO, Roughness, Metalness into a single RGBA PNG:
      R = AO.gray, G = Roughness.gray, B = Metalness.gray, A = 255

    Follows the glTF ORM convention (R=occlusion, G=roughness, B=metallic).
    All input images are converted to grayscale before packing.
    Saves result to out_png.
    """
    try:
        from PIL import Image
    except ImportError:
        print('ERROR: Pillow not installed. pip install Pillow', file=sys.stderr)
        sys.exit(1)

    ao   = Image.open(ao_path).convert('L')
    rou  = Image.open(roughness_path).convert('L')
    met  = Image.open(metalness_path).convert('L')

    # Verify all same size (warn if not, resize to AO size)
    w, h = ao.size
    if rou.size != (w, h):
        print(f'  WARNING: Roughness {rou.size} != AO {ao.size}, resizing', file=sys.stderr)
        rou = rou.resize((w, h), Image.LANCZOS)
    if met.size != (w, h):
        print(f'  WARNING: Metalness {met.size} != AO {ao.size}, resizing', file=sys.stderr)
        met = met.resize((w, h), Image.LANCZOS)

    from PIL import Image as _Image
    alpha = _Image.new('L', (w, h), 255)

    orm = _Image.merge('RGBA', (ao, rou, met, alpha))
    orm.save(out_png, format='PNG')
    print(f'  Packed ORM ({w}x{h}): R=AO G=Roughness B=Metalness A=255')


# ---------------------------------------------------------------------------
# TERRAIN-MATERIAL-TEXTURE-REMAP-1 prep: PolyHaven/EXR-aware source normalization
# ---------------------------------------------------------------------------
#
# The AmbientCG packs above are already _Color.png/_NormalGL.png (8-bit PNG) --
# cook_png_to_bc7() handles those directly. The PolyHaven-format terrain packs
# (rocks_ground_*, dirt_aerial_03, snow_field_aerial, ...) ship normal/roughness
# maps as EXR (often DWAA-compressed, which ktx.exe's own EXR reader rejects)
# and albedo as JPG. normalize_to_png() ingests any of PNG/JPG/EXR via cv2
# (already a repo dependency; handles DWAA-compressed EXR where ktx.exe's own
# reader fails) and re-encodes to a plain 8-bit PNG so the existing
# cook_png_to_bc7() two-step (ktx create --encode uastc -> ktx transcode bc7)
# never has to see anything but PNG.

def normalize_to_png(src_path, out_png, srgb):
    """
    Load src_path (PNG/JPG/EXR) via cv2 and re-save as an 8-bit PNG at out_png.

    srgb=True:  source is assumed already display-referred (color/albedo) --
                clamp to [0,1] and quantize to 8-bit as-is (no OETF applied;
                JPG/PNG albedo sources are already sRGB-encoded bytes).
    srgb=False: source is linear data (normal/roughness/height) -- clamp to
                [0,1] and quantize to 8-bit directly (no linear->sRGB curve;
                this matches the UNORM/linear treatment cook_png_to_bc7()
                already gives normal/orm maps).

    Returns out_png (str) on success. Raises RuntimeError on load failure.
    """
    # opencv-python(-headless) ships OpenEXR support disabled by default
    # (CVE-2021-XXXX mitigation upstream); must opt in before cv2 touches any
    # EXR codec path. Must be set before the *first* cv2 import in the process.
    os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
    import numpy as np
    import cv2

    img = cv2.imread(src_path, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError(f'cv2 failed to load {src_path}')

    # cv2 loads EXR/float sources as float32 in [0, inf); 8-bit PNG/JPG as uint8.
    if img.dtype != np.uint8:
        img = np.clip(img, 0.0, 1.0)
        img = (img * 255.0 + 0.5).astype(np.uint8)

    # cv2 is BGR(A); convert to RGB(A) for PIL-consistent channel order.
    if img.ndim == 2:
        img = cv2.cvtColor(img, cv2.COLOR_GRAY2RGB)
    elif img.shape[2] == 4:
        img = cv2.cvtColor(img, cv2.COLOR_BGRA2RGBA)
    elif img.shape[2] == 3:
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

    from PIL import Image
    mode = 'RGBA' if img.shape[2] == 4 else 'RGB'
    Image.fromarray(img, mode=mode).save(out_png, format='PNG')
    return out_png


def pack_height_into_normal_alpha(normal_path, height_path, out_png, size):
    """
    Build an RGBA PNG: rgb = normal_path's RGB (tangent-space normal,
    NormalGL/_nor_gl convention), a = height_path's grayscale (displacement).

    Matches the engine's matNormalArray convention where .rgb is the tangent
    normal and .a is displacement/height (terrain_lod_chunk.frag:274-276,827;
    terrtxm2.cpp:2566-2571 cpuDispAlphaSize retention).

    height_path may be None -- alpha is filled with 255 (flat/no displacement)
    in that case (e.g. marble_cliff_01 ships no height map; cliff path uses
    triplanar sampling, not POM).

    Both inputs are resized to `size` x `size` (engine requires one square
    arrayWidth shared by every layer, terrtxm2.cpp:2491-2492).
    """
    from PIL import Image

    normal_img = Image.open(normal_path).convert('RGB').resize((size, size), Image.LANCZOS)
    r, g, b = normal_img.split()

    if height_path:
        height_img = Image.open(height_path).convert('L').resize((size, size), Image.LANCZOS)
        a = height_img
    else:
        a = Image.new('L', (size, size), 255)

    Image.merge('RGBA', (r, g, b, a)).save(out_png, format='PNG')
    return out_png


def resize_to_square(src_path, out_png, size):
    """Load src_path (any PIL-supported mode) and resize/save to size x size PNG."""
    from PIL import Image

    img = Image.open(src_path).convert('RGBA').resize((size, size), Image.LANCZOS)
    img.save(out_png, format='PNG')
    return out_png


def cook_source_to_bc7(ktx_exe, src_png, out_ktx2, vk_format, oetf, tmpdir, mips=False):
    """Thin re-export of cook_png_to_bc7 for callers that only have a plain PNG."""
    return cook_png_to_bc7(ktx_exe, src_png, out_ktx2, vk_format, oetf, tmpdir, mips=mips)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Cook AmbientCG PBR material pack to BC7 KTX2 textures.'
    )
    parser.add_argument('--input-dir', required=True,
                        help='Directory containing AmbientCG PNGs')
    parser.add_argument('--out-dir', required=True,
                        help='Output directory for KTX2 files')
    parser.add_argument('--name', required=True,
                        help='Output filename slug (e.g. corrugatedsteel006a)')
    parser.add_argument('--force', action='store_true',
                        help='Re-cook even if output already exists')
    parser.add_argument('--ktx-exe', default=DEFAULT_KTX_EXE,
                        help=f'Path to ktx.exe (default: {DEFAULT_KTX_EXE})')
    args = parser.parse_args()

    ktx_exe = args.ktx_exe
    if not os.path.exists(ktx_exe):
        print(f'ERROR: ktx.exe not found at {ktx_exe}', file=sys.stderr)
        sys.exit(1)

    input_dir = os.path.abspath(args.input_dir)
    out_dir   = os.path.abspath(args.out_dir)
    slug      = args.name

    if not os.path.isdir(input_dir):
        print(f'ERROR: --input-dir not found: {input_dir}', file=sys.stderr)
        sys.exit(1)

    os.makedirs(out_dir, exist_ok=True)

    # Detect maps
    maps = detect_maps(input_dir)
    print('Detected maps:')
    for k, v in maps.items():
        status = os.path.basename(v) if v else 'NOT FOUND'
        print(f'  {k:12s}: {status}')

    results = {}   # slug -> (out_path, success)
    errors  = []

    with tempfile.TemporaryDirectory() as tmpdir:

        # ------------------------------------------------------------------
        # Albedo: sRGB BC7
        # ------------------------------------------------------------------
        albedo_out = os.path.join(out_dir, f'{slug}_albedo.ktx2')
        if maps['albedo']:
            if not args.force and os.path.exists(albedo_out):
                print(f'\n[albedo] SKIP (already exists, use --force to re-cook)')
                results['albedo'] = (albedo_out, True)
            else:
                print(f'\n[albedo] Cooking {os.path.basename(maps["albedo"])} -> {slug}_albedo.ktx2')
                ok = cook_png_to_bc7(
                    ktx_exe, maps['albedo'], albedo_out,
                    vk_format='R8G8B8A8_SRGB', oetf='srgb',
                    tmpdir=tmpdir,
                )
                results['albedo'] = (albedo_out, ok)
                if not ok:
                    errors.append('albedo')
        else:
            print('\n[albedo] SKIPPED (no _Color.png found)')

        # ------------------------------------------------------------------
        # Normal: linear BC7 (NormalGL only)
        # ------------------------------------------------------------------
        normal_out = os.path.join(out_dir, f'{slug}_normal.ktx2')
        if maps['normal']:
            if not args.force and os.path.exists(normal_out):
                print(f'\n[normal] SKIP (already exists, use --force to re-cook)')
                results['normal'] = (normal_out, True)
            else:
                print(f'\n[normal] Cooking {os.path.basename(maps["normal"])} -> {slug}_normal.ktx2')
                ok = cook_png_to_bc7(
                    ktx_exe, maps['normal'], normal_out,
                    vk_format='R8G8B8A8_UNORM', oetf='linear',
                    tmpdir=tmpdir,
                )
                results['normal'] = (normal_out, ok)
                if not ok:
                    errors.append('normal')
        else:
            print('\n[normal] SKIPPED (no _NormalGL.png found)')

        # ------------------------------------------------------------------
        # ORM: pack then cook as linear BC7
        # ------------------------------------------------------------------
        orm_out     = os.path.join(out_dir, f'{slug}_orm.ktx2')
        orm_src_png = os.path.join(out_dir, f'{slug}_orm_src.png')
        orm_maps_present = maps['ao'] and maps['roughness'] and maps['metalness']

        if orm_maps_present:
            if not args.force and os.path.exists(orm_out):
                print(f'\n[orm] SKIP (already exists, use --force to re-cook)')
                results['orm'] = (orm_out, True)
            else:
                print(f'\n[orm] Packing AO + Roughness + Metalness ...')
                pack_orm(maps['ao'], maps['roughness'], maps['metalness'], orm_src_png)

                print(f'[orm] Cooking {slug}_orm_src.png -> {slug}_orm.ktx2')
                ok = cook_png_to_bc7(
                    ktx_exe, orm_src_png, orm_out,
                    vk_format='R8G8B8A8_UNORM', oetf='linear',
                    tmpdir=tmpdir,
                )
                results['orm'] = (orm_out, ok)
                if not ok:
                    errors.append('orm')

                # Clean up temp ORM source PNG
                try:
                    os.remove(orm_src_png)
                    print(f'  Removed temp ORM source PNG')
                except OSError as e:
                    print(f'  WARNING: could not remove {orm_src_png}: {e}', file=sys.stderr)
        else:
            missing = [k for k in ('ao', 'roughness', 'metalness') if not maps[k]]
            print(f'\n[orm] SKIPPED (missing: {", ".join(missing)})')

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print('\n' + '='*60)
    print('Cook summary:')
    for label, (path, ok) in results.items():
        if os.path.exists(path):
            size = os.path.getsize(path)
            status = 'OK' if ok else 'FAILED'
            print(f'  {label:8s}: {path}')
            print(f'           {size:,} bytes  [{status}]')
        else:
            print(f'  {label:8s}: MISSING  [FAILED]')

    if errors:
        print(f'\nERRORS in: {", ".join(errors)}', file=sys.stderr)
        sys.exit(1)
    else:
        print('\nAll maps cooked successfully.')


if __name__ == '__main__':
    main()
