#!/usr/bin/env python3
"""
cook_bc6h_hdri.py -- Cook an EXR HDRI into a BC6H_UFLOAT_BLOCK KTX2 file.

Uses texconv.exe (Microsoft DirectXTex) for GPU-accelerated BC6H encoding,
then wraps the raw blocks with ktx.exe --raw to produce a standards-compliant
KTX2 file.

Pipeline:
  EXR (OpenEXR)  ->  Radiance HDR (tmp)  ->  texconv BC6H_UF16 DDS (tmp)
  ->  strip 148-byte DDS header  ->  ktx.exe --raw  ->  .ktx2

VK_FORMAT_BC6H_UFLOAT_BLOCK = 143 (Vulkan 1.4 spec)

Usage:
    python cook_bc6h_hdri.py <input.exr> <output.ktx2> [<path_to_ktx.exe>]

External tools required (defaults):
    texconv.exe  -- A:\\Games\\mc2-tools\\texconv\\texconv.exe
    ktx.exe      -- A:\\Games\\mc2-tools\\ktx\\ktx.exe
"""

import sys
import os
import subprocess
import tempfile
import time
import struct
import io
import numpy as np
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

try:
    import OpenEXR
    import Imath
except ImportError:
    print("ERROR: OpenEXR Python module required. Install via: pip install openexr", file=sys.stderr)
    sys.exit(1)

# Default tool paths
DEFAULT_KTX_EXE     = r'A:\Games\mc2-tools\ktx\ktx.exe'
DEFAULT_TEXCONV_EXE = r'A:\Games\mc2-tools\texconv\texconv.exe'

VK_FORMAT_BC6H_UFLOAT_BLOCK = 143  # Vulkan spec

# DDS header size when DX10 extension is present: magic(4) + DDS_HEADER(124) + DDS_HEADER_DXT10(20)
DDS_HEADER_SIZE = 148


def exr_to_radiance_hdr(exr_path):
    """
    Read an EXR file and return (width, height, bytes) for a Radiance HDR (.hdr).

    Radiance HDR uses RGBE (4 bytes/pixel, no RLE) -- simple, lossless, and
    accepted by texconv as a float-precision input format.

    RGBE encoding: mantissa in RGB (0..255), shared exponent E = exp+128.
    Decoding: R_f = R/256 * 2^(E-128).  Precision: ~8 mantissa bits per channel,
    sufficient for texconv's GPU encoder which operates at float32 internally.
    """
    exr = OpenEXR.InputFile(exr_path)
    hdr = exr.header()
    dw = hdr['dataWindow']
    w = dw.max.x - dw.min.x + 1
    h = dw.max.y - dw.min.y + 1

    pt = Imath.PixelType(Imath.PixelType.FLOAT)
    R = np.frombuffer(exr.channel('R', pt), dtype=np.float32).reshape(h, w)
    G = np.frombuffer(exr.channel('G', pt), dtype=np.float32).reshape(h, w)
    B = np.frombuffer(exr.channel('B', pt), dtype=np.float32).reshape(h, w)

    # Stack and encode as RGBE
    pixels = np.stack([R, G, B], axis=2)  # (h, w, 3) float32

    eps = 1e-9
    max_c = np.maximum(pixels.max(axis=2), eps)          # (h, w)
    exp   = np.floor(np.log2(max_c)).astype(np.int32) + 1
    scale = np.ldexp(1.0, -exp)[:, :, np.newaxis]        # (h, w, 1)

    mantissa = np.clip((pixels * scale * 256), 0, 255).astype(np.uint8)  # (h, w, 3)
    exp_byte = np.clip(exp + 128, 0, 255).astype(np.uint8)               # (h, w)
    rgbe = np.concatenate([mantissa, exp_byte[:, :, np.newaxis]], axis=2)  # (h, w, 4)

    buf = io.BytesIO()
    buf.write(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n')
    buf.write(f'-Y {h} +X {w}\n'.encode('ascii'))
    buf.write(rgbe.tobytes())

    return w, h, buf.getvalue()


def run_texconv_bc6h(hdr_path, out_dir, texconv_exe):
    """Run texconv to encode a .hdr file to BC6H_UF16 DDS. Returns the DDS path."""
    cmd = [
        texconv_exe,
        '-f', 'BC6H_UF16',
        '-o', out_dir,
        '-y',
        '-m', '1',    # single mip level
        hdr_path,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if result.returncode != 0:
        print(f"texconv failed (rc={result.returncode}):", file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        sys.exit(1)

    base = os.path.splitext(os.path.basename(hdr_path))[0]
    dds_path = os.path.join(out_dir, base + '.dds')
    if not os.path.exists(dds_path):
        print(f"texconv succeeded but DDS not found at {dds_path}", file=sys.stderr)
        print("texconv output:", result.stdout, file=sys.stderr)
        sys.exit(1)
    return dds_path


def strip_dds_header(dds_path, expected_w, expected_h):
    """
    Read a DDS file produced by texconv, validate the header, and return the
    raw BC6H block data (everything after the DDS header).

    DDS layout (DX10 extension present):
        magic(4) + DDS_HEADER(124) + DDS_HEADER_DXT10(20) = 148 bytes header
    """
    with open(dds_path, 'rb') as f:
        data = f.read()

    if len(data) < DDS_HEADER_SIZE:
        print(f"DDS too small: {len(data)} bytes", file=sys.stderr)
        sys.exit(1)

    magic = data[:4]
    if magic != b'DDS ':
        print(f"DDS magic mismatch: {magic!r}", file=sys.stderr)
        sys.exit(1)

    dds_h_size = struct.unpack_from('<I', data, 4)[0]
    if dds_h_size != 124:
        print(f"Unexpected DDS_HEADER size: {dds_h_size} (expected 124)", file=sys.stderr)
        sys.exit(1)

    h_h   = struct.unpack_from('<I', data, 12)[0]
    w_h   = struct.unpack_from('<I', data, 16)[0]
    mips  = struct.unpack_from('<I', data, 28)[0]
    fourcc = data[84:88]

    if fourcc != b'DX10':
        print(f"Expected DX10 FourCC, got: {fourcc!r}", file=sys.stderr)
        sys.exit(1)

    dxgi_fmt = struct.unpack_from('<I', data, 128)[0]
    if dxgi_fmt != 95:
        print(f"DXGI format {dxgi_fmt} is not BC6H_UF16 (95)", file=sys.stderr)
        sys.exit(1)

    if w_h != expected_w or h_h != expected_h:
        print(f"DDS dimensions {w_h}x{h_h} != expected {expected_w}x{expected_h}", file=sys.stderr)
        sys.exit(1)

    raw_blocks = data[DDS_HEADER_SIZE:]
    expected_raw = ((expected_w + 3) // 4) * ((expected_h + 3) // 4) * 16
    if len(raw_blocks) != expected_raw:
        print(f"Raw block size {len(raw_blocks)} != expected {expected_raw}", file=sys.stderr)
        sys.exit(1)

    print(f"DDS validated: {w_h}x{h_h} BC6H_UF16, {mips} mip, "
          f"{len(raw_blocks)/(1024*1024):.2f} MB raw blocks")
    return raw_blocks


def sanity_check_blocks(raw_blocks, w, h, n_samples=5):
    """
    Sanity-check n_samples random BC6H blocks from the raw block data.

    BC6H blocks use many different modes (0x00-0x1F) each with distinct
    endpoint layouts -- full per-mode decoding is complex.  Instead we apply
    two lightweight checks that reliably catch the 'pure black' failure class:

    1. Non-zero check: at least one of the 16 bytes in the block is non-zero.
       A pure-black or all-zero encoder bug produces 16 zero bytes per block.

    2. Mode-11 fast decode (only for blocks whose mode bits == 0x1F):
       mode 11 has 10-bit absolute endpoints; dequantize and range-check.
       texconv emits mode 11 for near-uniform low-variance blocks (sky patches),
       so we'll see at least some of these in an HDR sky image.

    Reports block index, grid position, mode, and first/last bytes.
    """
    import random
    n_blocks = len(raw_blocks) // 16
    bw = (w + 3) // 4

    print(f"Sanity check: sampling {n_samples} random blocks from {n_blocks} total...")
    all_ok = True
    rng = random.Random(42)

    for s in range(n_samples):
        bi = rng.randint(0, n_blocks - 1)
        block = raw_blocks[bi*16:(bi+1)*16]
        bx = bi % bw
        by = bi // bw

        # Check 1: non-zero
        if all(b == 0 for b in block):
            print(f"  block[{bi}] ({bx},{by}) -> FAIL (all-zero bytes)", file=sys.stderr)
            all_ok = False
            continue

        bits  = int.from_bytes(block, 'little')
        mode5 = bits & 0x1F
        first4 = ' '.join(f'{block[i]:02x}' for i in range(4))
        last4  = ' '.join(f'{block[i]:02x}' for i in range(12, 16))

        # Check 2: mode-11 endpoint range (only for mode==0x1F)
        mode_note = f'mode={mode5:#04x}'
        if mode5 == 0x1F:
            rw0 = (bits >> 5)  & 0x3FF
            rw1 = (bits >> 15) & 0x3FF
            gw0 = (bits >> 25) & 0x3FF
            gw1 = (bits >> 35) & 0x3FF
            bw0 = (bits >> 45) & 0x3FF
            bw1 = (bits >> 55) & 0x3FF

            def ep10_to_f32(ep10):
                h16 = np.uint16(ep10 << 6)
                return float(h16.view(np.float16))

            vals = [ep10_to_f32(v) for v in [rw0, rw1, gw0, gw1, bw0, bw1]]
            in_range = all(0.0 <= v <= 65504.0 for v in vals)
            not_all_zero = any(v > 0.0 for v in vals)
            mode_note = (f'mode=0x1F R({vals[0]:.1f},{vals[1]:.1f}) '
                         f'G({vals[2]:.1f},{vals[3]:.1f}) B({vals[4]:.1f},{vals[5]:.1f})')
            if not (in_range and not_all_zero):
                print(f"  block[{bi}] ({bx},{by}) {mode_note} -> FAIL", file=sys.stderr)
                all_ok = False
                continue

        print(f"  block[{bi}] ({bx},{by}) {mode_note} "
              f"bytes=[{first4}..{last4}] -> OK")

    if not all_ok:
        print("ERROR: sanity check failed -- pure-black or out-of-range blocks detected",
              file=sys.stderr)
        sys.exit(1)
    print("Sanity check PASSED")


def cook(exr_path, ktx2_path, ktx_exe, texconv_exe):
    """Cook EXR -> BC6H KTX2 via texconv GPU encoder."""
    print(f"Input EXR: {exr_path}", flush=True)
    cook_start = time.perf_counter()

    # Step 1: EXR -> Radiance HDR (lossless RGBE, in-memory)
    print("Converting EXR -> Radiance HDR...", flush=True)
    t0 = time.perf_counter()
    w, h, hdr_bytes = exr_to_radiance_hdr(exr_path)
    t1 = time.perf_counter()
    print(f"  {w}x{h}, {len(hdr_bytes)/(1024*1024):.1f} MB HDR ({t1-t0:.1f}s)", flush=True)

    # Step 2: texconv GPU-encodes HDR -> BC6H_UF16 DDS
    with tempfile.TemporaryDirectory() as tmpdir:
        hdr_tmp = os.path.join(tmpdir, 'hdri_tmp.hdr')
        with open(hdr_tmp, 'wb') as f:
            f.write(hdr_bytes)
        del hdr_bytes  # free ~32 MB

        print("Running texconv (GPU BC6H encode)...", flush=True)
        t0 = time.perf_counter()
        dds_path = run_texconv_bc6h(hdr_tmp, tmpdir, texconv_exe)
        t1 = time.perf_counter()
        print(f"  texconv done in {t1-t0:.1f}s", flush=True)

        # Step 3: Strip DDS header -> raw BC6H blocks
        raw_blocks = strip_dds_header(dds_path, w, h)

        # Step 4: Sanity-check a few decoded blocks
        sanity_check_blocks(raw_blocks, w, h, n_samples=5)

        # Step 5: Write raw blocks to temp file and create KTX2 via ktx.exe --raw
        raw_tmp = os.path.join(tmpdir, 'blocks.raw')
        with open(raw_tmp, 'wb') as f:
            f.write(raw_blocks)
        del raw_blocks  # free ~8 MB

        print("Creating KTX2 via ktx.exe --raw...", flush=True)
        cmd = [
            ktx_exe,
            'create',
            '--format', 'BC6H_UFLOAT_BLOCK',
            '--raw',
            '--width',  str(w),
            '--height', str(h),
            '--assign-tf', 'linear',
            raw_tmp,
            ktx2_path,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        if result.returncode != 0:
            print(f"ktx.exe error (rc={result.returncode}): {result.stderr}", file=sys.stderr)
            sys.exit(1)

    out_bytes = os.path.getsize(ktx2_path)
    cook_elapsed = time.perf_counter() - cook_start
    print(
        f"[cook_bc6h] input: {w}x{h}  "
        f"output: {out_bytes} bytes ({out_bytes / (1024*1024):.0f} MB)  "
        f"time: {cook_elapsed:.1f}s",
        flush=True,
    )


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.exr> <output.ktx2> [ktx.exe [texconv.exe]]",
              file=sys.stderr)
        sys.exit(1)

    exr_path    = sys.argv[1]
    ktx2_path   = sys.argv[2]
    ktx_exe     = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_KTX_EXE
    texconv_exe = sys.argv[4] if len(sys.argv) > 4 else DEFAULT_TEXCONV_EXE

    for tool, name in [(ktx_exe, 'ktx.exe'), (texconv_exe, 'texconv.exe')]:
        if not os.path.exists(tool):
            print(f"ERROR: {name} not found at {tool}", file=sys.stderr)
            sys.exit(1)

    cook(exr_path, ktx2_path, ktx_exe, texconv_exe)

    # Run ktx info to verify output
    result = subprocess.run([ktx_exe, 'info', ktx2_path], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    for line in result.stdout.split('\n')[:20]:
        print(line)


if __name__ == '__main__':
    main()
