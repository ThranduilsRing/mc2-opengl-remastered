#!/usr/bin/env python3
"""tools/mc2texcook/mc2texcook.py -- MC2 OpenGL texture cook tool.

Converts TGA/PNG/EXR/TXM source art to KTX2 (RGBA8, uncompressed, full mip chain).
Requires: Pillow (pip install Pillow).

Usage:
    py -3 mc2texcook.py <input> --preset <albedo|normal|orm|emissive|mask> [--output <out.ktx2>]

Presets:
    albedo   -- RGBA8 sRGB  (VK_FORMAT_R8G8B8A8_SRGB = 43), full mip chain
    emissive -- same as albedo (sRGB)
    normal   -- RGBA8 UNORM (VK_FORMAT_R8G8B8A8_UNORM = 37), no color space conversion
    orm      -- RGBA8 UNORM, pack R=AO G=roughness B=metalness A=255
    mask     -- RGBA8 UNORM, grayscale -> R=G=B=value A=255; pass-through if already RGBA

TXM input:
    .txm files are MC2 static-prop textures: 64x64 BGRA raw pixels, Windows LZW-compressed.
    The cook tool decompresses and swaps B<->R to produce RGBA (matching GL_RGBA upload order
    used by the KTX2 runtime sidecar path in gos_static_prop_batcher.cpp).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# KTX2 constants
# ---------------------------------------------------------------------------

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"  # 12 bytes

VK_FORMAT_R8G8B8A8_UNORM = 37
VK_FORMAT_R8G8B8A8_SRGB  = 43

# ---------------------------------------------------------------------------
# Pre-built DFDs (80 bytes each, including the leading dfdTotalSize uint32)
# ---------------------------------------------------------------------------

# KHR Basic Descriptor Block for RGBA8 -- 92 bytes total.
#
# Layout (all LE):
#   offset  0: dfdTotalSize = 92  (uint32)  -- includes this field
#   offset  4: vendorId=0 (uint16) | descriptorType=0 (uint16)
#   offset  8: versionNumber=2 (uint16) | descriptorBlockSize=80 (uint16)
#                descriptorBlockSize counts bytes from colorModel through end of samples
#   offset 12: colorModel=1(RGBSDA), colorPrimaries=1(BT709), transferFunction, flags=0
#   offset 16: texelBlockDimension[0-3] = 0,0,0,0
#   offset 20: bytesPlane[0-7] = 4,0,0,0,0,0,0,0
#   offset 28: 4 samples x 16 bytes = 64 bytes
#                each: bitOffset(uint16) bitLength(uint8) channelType(uint8)
#                      samplePosition(uint32) lower(int32) upper(int32)
#
DFD_RGBA8_UNORM = bytes([
    # dfdTotalSize = 92 (0x5C)
    0x5C, 0x00, 0x00, 0x00,
    # vendorId=0, descriptorType=0
    0x00, 0x00, 0x00, 0x00,
    # versionNumber=2 (uint16), descriptorBlockSize=80 (0x50) (uint16)
    0x02, 0x00, 0x50, 0x00,
    # colorModel=1(RGBSDA), colorPrimaries=1(BT709), transferFunction=1(LINEAR), flags=0
    0x01, 0x01, 0x01, 0x00,
    # texelBlockDimension[0-3] = 0,0,0,0
    0x00, 0x00, 0x00, 0x00,
    # bytesPlane[0-7]: plane0=4, rest=0
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    # Sample 0 (R): bitOffset=0 (uint16), bitLength=7 (uint8), channelType=0x00 (R)
    0x00, 0x00, 0x07, 0x00,
    # samplePosition[0-3] = 0 (packed as uint32)
    0x00, 0x00, 0x00, 0x00,
    # lower=0 (int32), upper=255 (int32)
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    # Sample 1 (G): bitOffset=8
    0x08, 0x00, 0x07, 0x01,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    # Sample 2 (B): bitOffset=16
    0x10, 0x00, 0x07, 0x02,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    # Sample 3 (A): bitOffset=24, channelType=0x0F (alpha)
    0x18, 0x00, 0x07, 0x0F,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
])

# sRGB: same as UNORM but transferFunction byte (offset 14) = 2 (SRGB)
DFD_RGBA8_SRGB = bytearray(DFD_RGBA8_UNORM)
DFD_RGBA8_SRGB[14] = 0x02
DFD_RGBA8_SRGB = bytes(DFD_RGBA8_SRGB)

assert len(DFD_RGBA8_UNORM) == 92, f"DFD_RGBA8_UNORM is {len(DFD_RGBA8_UNORM)} bytes, expected 92"
assert len(DFD_RGBA8_SRGB)  == 92, f"DFD_RGBA8_SRGB is {len(DFD_RGBA8_SRGB)} bytes, expected 92"

# ---------------------------------------------------------------------------
# TXM (MC2 static-prop texture) reader
# ---------------------------------------------------------------------------

def _lzw_decomp(data: bytes, max_out: int = 262144) -> bytes:
    """Variable-width LZW decompressor (9-12 bit, LSB-first).

    Matches the MC2 Windows assembly LZW in mclib/lzdecomp.cpp:
      clear=256, eof=257, first_free=258, max_dict=4096.
    Bit packing is LSB-first (lowest bit of each code goes into bit BitOffset
    of the current output byte, advancing by ceil(bits/8) as encoded by the asm).
    """
    code_size = 9
    max_code = 1 << code_size   # 512
    table: dict[int, bytes] = {i: bytes([i]) for i in range(256)}
    next_code = 258
    bits = 0
    bit_count = 0
    pos = 0

    def read_code(n: int) -> int:
        nonlocal bits, bit_count, pos
        while bit_count < n:
            if pos < len(data):
                bits |= data[pos] << bit_count
                pos += 1
            bit_count += 8
        code = bits & ((1 << n) - 1)
        bits >>= n
        bit_count -= n
        return code

    out = bytearray()
    prev: bytes | None = None
    while pos < len(data) or bit_count >= code_size:
        code = read_code(code_size)
        if code == 257:
            break
        if code == 256:
            table = {i: bytes([i]) for i in range(256)}
            next_code = 258
            code_size = 9
            max_code = 512
            prev = None
            continue

        if code < next_code:
            entry = table[code]
        elif code == next_code and prev is not None:
            # KwKwK: code not yet in dict → prev + first byte of prev
            entry = prev + prev[:1]
        else:
            raise ValueError(f"LZW bad code {code} (next_code={next_code})")

        out.extend(entry)
        if len(out) >= max_out:
            break

        if prev is not None and next_code < 4096:
            table[next_code] = prev + entry[:1]
            next_code += 1
            if next_code >= max_code and code_size < 12:
                code_size += 1
                max_code = 1 << code_size
        prev = entry

    return bytes(out)


# All MC2 static-prop .txm files are 64×64 BGRA (4 bytes/pixel), LZW-compressed.
_TXM_SIDE = 64
_TXM_RAW_SIZE = _TXM_SIDE * _TXM_SIDE * 4  # 16384


def _read_txm(path: Path, output_size: int = 0):
    """Decompress a .txm file and return a PIL RGBA Image.

    TXM on-disk = Windows LZW-compressed 64×64 BGRA raw pixels (no header).
    We swap B↔R so the output matches GL_RGBA upload order expected by
    gos_static_prop_batcher.cpp's KTX2 sidecar path.

    output_size: if > 0, resize the decoded 64×64 image to output_size × output_size
    using LANCZOS.  The MC2 game engine upscales TXMs to ObjectTextureSize (typically
    128) when uploading to GL, so the KTX2 sidecar must match that uploaded size.
    output_size=0 (default) returns native 64×64.
    """
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        sys.exit("ERROR: Pillow is required.  Install with:  pip install Pillow")

    raw = path.read_bytes()
    pixels = _lzw_decomp(raw, max_out=_TXM_RAW_SIZE + 1)
    if len(pixels) != _TXM_RAW_SIZE:
        raise ValueError(
            f"TXM decompress: expected {_TXM_RAW_SIZE} bytes, got {len(pixels)}"
        )

    # Raw bytes are BGRA; create as RGBA then swap B↔R channels.
    img_bgra = Image.frombytes("RGBA", (_TXM_SIDE, _TXM_SIDE), pixels)
    b, g, r, a = img_bgra.split()
    img = Image.merge("RGBA", (r, g, b, a))

    if output_size > 0 and output_size != _TXM_SIDE:
        img = img.resize((output_size, output_size), Image.LANCZOS)
    return img


# ---------------------------------------------------------------------------
# Image loading and preset transforms
# ---------------------------------------------------------------------------

def _load_image(path: Path, txm_size: int = 0):
    """Load an image via Pillow (TGA/PNG/EXR) or decompress a .txm.

    txm_size: passed to _read_txm as output_size (0 = native 64×64).
    Returns a PIL Image object.
    """
    if path.suffix.lower() == ".txm":
        return _read_txm(path, output_size=txm_size)
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        sys.exit("ERROR: Pillow is required.  Install with:  pip install Pillow")
    img = Image.open(path)
    img.load()
    return img


def _apply_preset(img, preset: str):
    """Return (rgba_image, vk_format, dfd_bytes).

    rgba_image -- PIL Image in RGBA mode, ready for mip generation
    vk_format  -- VK_FORMAT_* constant
    dfd_bytes  -- pre-built DFD block
    """
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        sys.exit("ERROR: Pillow is required.  Install with:  pip install Pillow")

    if preset in ("albedo", "emissive"):
        # sRGB.  Pillow's convert('RGBA') preserves source channel data;
        # we treat the stored bytes as sRGB-encoded (i.e. no linearisation).
        rgba = img.convert("RGBA")
        return rgba, VK_FORMAT_R8G8B8A8_SRGB, DFD_RGBA8_SRGB

    elif preset == "normal":
        # Linear.  No color space conversion.
        rgba = img.convert("RGBA")
        return rgba, VK_FORMAT_R8G8B8A8_UNORM, DFD_RGBA8_UNORM

    elif preset == "orm":
        # Pack R=AO G=roughness B=metalness A=255.
        # Input expected to be a 3-channel (or 4-channel) image in R=AO/G=rough/B=metal order.
        rgb = img.convert("RGB")
        r, g, b = rgb.split()
        alpha = Image.new("L", rgb.size, 255)
        rgba = Image.merge("RGBA", (r, g, b, alpha))
        return rgba, VK_FORMAT_R8G8B8A8_UNORM, DFD_RGBA8_UNORM

    elif preset == "mask":
        # Grayscale -> RGBA with R=G=B=gray, A=255.
        # If already RGBA, pass through.
        if img.mode == "RGBA":
            return img.copy(), VK_FORMAT_R8G8B8A8_UNORM, DFD_RGBA8_UNORM
        gray = img.convert("L")
        rgba = Image.merge("RGBA", (gray, gray, gray, Image.new("L", gray.size, 255)))
        return rgba, VK_FORMAT_R8G8B8A8_UNORM, DFD_RGBA8_UNORM

    else:
        sys.exit(f"ERROR: unknown preset '{preset}'. "
                 "Choose: albedo, normal, orm, emissive, mask")


# ---------------------------------------------------------------------------
# Mip chain
# ---------------------------------------------------------------------------

def _generate_mips(base_img):
    """Return list of PIL Images: [mip0 (largest), mip1, ..., mipN (1x1)].

    Each level halves both dimensions (floor division), stopping at 1x1.

    RGB and alpha are downsampled INDEPENDENTLY (not as a single RGBA resize).
    Pillow's LANCZOS resize of an RGBA image premultiplies alpha, which zeroes
    the RGB of any fully-transparent texel — so source art that is transparent
    (alpha==0) but carries meaningful RGB (cliffs, fences, markers) would
    produce BLACK coarse mips and the prop turns black at distance. Resizing the
    RGB and alpha planes separately matches glGenerateMipmap's non-premultiplied
    per-channel filter and keeps the albedo correct regardless of alpha.
    """
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        sys.exit("ERROR: Pillow is required.  Install with:  pip install Pillow")

    mips = [base_img]
    w, h = base_img.size

    if base_img.mode == "RGBA":
        r, g, b, a = base_img.split()
        rgb = Image.merge("RGB", (r, g, b))
        while w > 1 or h > 1:
            w = max(1, w >> 1)
            h = max(1, h >> 1)
            rgb_l = rgb.resize((w, h), Image.LANCZOS)
            a_l = a.resize((w, h), Image.LANCZOS)
            rr, gg, bb = rgb_l.split()
            mips.append(Image.merge("RGBA", (rr, gg, bb, a_l)))
    else:
        while w > 1 or h > 1:
            w = max(1, w >> 1)
            h = max(1, h >> 1)
            mips.append(base_img.resize((w, h), Image.LANCZOS))
    return mips


# ---------------------------------------------------------------------------
# KTX2 writer
# ---------------------------------------------------------------------------

def write_ktx2(out_path: Path, mips, vk_format: int, dfd: bytes) -> None:
    """Write a valid KTX2 file (RGBA8, no supercompression) to out_path.

    KTX2 file layout (Khronos spec r7):
        offset   0 : magic (12 bytes)
        offset  12 : header fields (9 x uint32 = 36 bytes)
        offset  48 : index fields: dfdByteOffset, dfdByteLength,
                     kvdByteOffset, kvdByteLength (4 x uint32 = 16 bytes)
                     sgdByteOffset, sgdByteLength (2 x uint64 = 16 bytes)
        offset  80 : level index (levelCount x 24 bytes each)
        after idx  : DFD
        after DFD  : KV data (empty)
        after KV   : mip pixel data (mip 0 first, largest)
    """
    level_count = len(mips)
    width0, height0 = mips[0].size

    # --- Layout calculation ---
    HEADER_SIZE      = 80  # magic(12) + 9*uint32(36) + 4*uint32(16) + 2*uint64(16)
    level_index_size = level_count * 24
    dfd_start        = HEADER_SIZE + level_index_size
    dfd_size         = len(dfd)   # always 80
    kvd_start        = dfd_start + dfd_size
    kvd_size         = 0
    mip_data_start   = kvd_start + kvd_size  # no alignment padding needed for uncompressed

    # Collect pixel data for all mip levels.
    mip_bytes: list[bytes] = []
    for m in mips:
        mip_bytes.append(m.tobytes())  # raw RGBA8 row-major

    # Compute per-level byte offsets (absolute from start of file).
    level_offsets: list[int] = []
    offset = mip_data_start
    for b in mip_bytes:
        level_offsets.append(offset)
        offset += len(b)

    # --- Assemble header ---
    buf = bytearray()

    # Magic
    buf += KTX2_MAGIC

    # 9 x uint32
    buf += struct.pack(
        "<9I",
        vk_format,   # vkFormat
        1,           # typeSize
        width0,      # pixelWidth
        height0,     # pixelHeight
        0,           # pixelDepth
        0,           # layerCount
        1,           # faceCount
        level_count, # levelCount
        0,           # supercompressionScheme (NONE)
    )

    # Index: dfdByteOffset, dfdByteLength, kvdByteOffset, kvdByteLength (4 x uint32)
    buf += struct.pack("<4I", dfd_start, dfd_size, kvd_start, kvd_size)

    # sgdByteOffset, sgdByteLength (2 x uint64)
    buf += struct.pack("<2Q", 0, 0)

    assert len(buf) == HEADER_SIZE, f"header is {len(buf)} bytes, expected {HEADER_SIZE}"

    # --- Level index ---
    for i, mb in enumerate(mip_bytes):
        byte_len = len(mb)
        buf += struct.pack("<3Q",
                           level_offsets[i],  # byteOffset
                           byte_len,           # byteLength
                           byte_len)           # uncompressedByteLength

    assert len(buf) == dfd_start, f"buf is {len(buf)} at DFD start, expected {dfd_start}"

    # --- DFD ---
    buf += dfd

    # --- KV data (empty) ---
    # (nothing to append)

    # --- Pixel data ---
    for mb in mip_bytes:
        buf += mb

    out_path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Cook TGA/PNG/EXR source art to KTX2 RGBA8 (uncompressed, full mip chain).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("input", type=Path, help="Source image: TGA, PNG, EXR, or MC2 TXM")
    ap.add_argument("--preset", required=True,
                    choices=["albedo", "normal", "orm", "emissive", "mask"],
                    help="Texture usage preset (controls vkFormat and channel packing)")
    ap.add_argument("--output", type=Path, default=None,
                    help="Output path (default: same name as input with .ktx2 extension)")
    ap.add_argument("--size", type=int, default=0, metavar="N",
                    help="For .txm input: resize decoded 64x64 pixels to NxN before cooking "
                         "(use 128 to match MC2's ObjectTextureSize=128 GL upload size). "
                         "Default 0 = native 64x64.")
    args = ap.parse_args()

    in_path: Path = args.input.resolve()
    if not in_path.exists():
        print(f"ERROR: input file not found: {in_path}", file=sys.stderr)
        return 1

    out_path: Path = args.output.resolve() if args.output else in_path.with_suffix(".ktx2")

    img = _load_image(in_path, txm_size=args.size)
    rgba, vk_format, dfd = _apply_preset(img, args.preset)
    mips = _generate_mips(rgba)

    write_ktx2(out_path, mips, vk_format, dfd)

    w, h = mips[0].size
    fmt_name = "SRGB" if vk_format == VK_FORMAT_R8G8B8A8_SRGB else "UNORM"
    print(f"OK: {in_path.name} -> {out_path}  "
          f"({w}x{h}, RGBA8_{fmt_name}, {len(mips)} mips)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
