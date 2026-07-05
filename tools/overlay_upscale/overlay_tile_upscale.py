#!/usr/bin/env python3
"""overlay_tile_upscale.py -- OVERLAY-TILE-HIRES-1 asset cook.

Batch-upscales the 64x64 terrain overlay tiles (roads/runways/crossings/
bridges) to a higher power-of-two edge (default 256 = 4x) for the gated
engine loader (MC2_OVERLAY_TILE_HIRES, mclib/terrtxm.cpp initOverlay header
resync). Output goes to a `<edge>Overlays/` folder that the gated loader
probes first, with per-file fallback to the legacy `64Overlays/` tile --
partial sets are legal.

Sources (loose wins over FST, mirroring the engine's File::open order):
  1. <deploy>/data/textures/64Overlays/*.tga     (loose overrides)
  2. <deploy>/textures.fst  data/textures/64overlays/*  (zlib/LZW entries)

Upscale chain (house model):
  * RGB   -> 4x-UltraSharpV2.safetensors via spandrel/torch (CPU, deterministic)
  * Alpha -> Lanczos (the alpha IS the road shape mask; AI on a coverage mask
             risks hallucinated shape changes, Lanczos is faithful)

COLORKEY SAFETY (ROAD-MARKINGS-VECTOR-RECON-1 post-mortem cause #2):
  MC2's legacy transparent colorkey is magenta 0xFF00FF, and the engine's
  transition bake chroma-zeroes anything near-magenta at upload
  (terrtxm.cpp textureFromMemoryAlpha: r>150 && b>150 && r>g+40 && b>g+40).
  Raster-upscaling a keyed source multiplies near-magenta fringe pixels.
  Handling here:
    * pre-upscale: chroma-magenta pixels are masked out and inpainted with
      the nearest non-magenta color so the AI never sees magenta;
    * post-upscale: the (bilinear+threshold) upscaled mask region is restored
      to EXACT 0xFF00FF with alpha 0;
    * anti-hallucination clamp: any remaining output pixel that would pass
      the engine's chroma test gets its green channel raised just enough to
      fail it (so the bake path can never silently erase road art).
  NOTE: a full scan (2026-07-01) found ZERO chroma-magenta pixels in all 475
  FST + 33 loose stock overlay tiles (transparent regions carry dirt-colored
  RGB under alpha=0), so on stock input the mask path is a no-op -- it is
  kept for modder tiles that do use the colorkey.

Usage:
  py -3 tools/overlay_upscale/overlay_tile_upscale.py \
      --deploy "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0" \
      --out-dir "<deploy>/data/textures/256Overlays" \
      [--edge 256] [--families 2lanedirt,bridge,...] [--limit N]
      [--contact-sheet PATH] [--model PATH]

Deps: numpy, Pillow, torch, spandrel (model inference).
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
import zlib
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "mc2x_import"))
import fst as fstmod  # noqa: E402  (proven FST reader/LZW port)

DEFAULT_MODEL = r"A:/Games/mc2-opengl-src/esrgan_models/4x-UltraSharpV2.safetensors"

# Road/marking families to upscale. cementpattern* (cement transition
# machinery) and scorch* (FX decals) are deliberately excluded.
DEFAULT_FAMILIES = [
    "2lanedirt", "bridge", "bridgedam", "damagedroad", "dirtroad",
    "droad2proad", "droad2proadcross", "pavedroad", "rough", "runway",
    "x_2lanedirt_damaged", "x_2lanedirt_paved", "x_damaged_paved",
    "x_dirt_2lanedirt", "x_dirt_damaged", "x_dirt_paved",
    "x_pavedroad_bridge",
]


# --- TGA (uncompressed 32bpp truecolor only -- all overlay tiles are UNC_TRUE/32) ---

def tga_read(buf: bytes):
    """Return (desc, HxWx4 uint8 BGRA in file row order)."""
    idlen, cmap, imgtype = buf[0], buf[1], buf[2]
    w, h = struct.unpack_from("<HH", buf, 12)
    depth, desc = buf[16], buf[17]
    if imgtype != 2 or depth != 32:
        raise ValueError(f"unsupported TGA (type={imgtype} depth={depth})")
    px = np.frombuffer(buf, dtype=np.uint8, count=w * h * 4,
                       offset=18 + idlen).reshape(h, w, 4).copy()
    return desc, px


def tga_write(path: Path, px: np.ndarray, desc: int) -> None:
    """Write uncompressed 32bpp truecolor TGA, preserving the origin/alpha
    descriptor byte of the source tile."""
    h, w = px.shape[:2]
    hdr = bytearray(18)
    hdr[2] = 2                      # UNC_TRUE
    struct.pack_into("<HH", hdr, 12, w, h)
    hdr[16] = 32
    hdr[17] = desc
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(hdr) + px.astype(np.uint8).tobytes())


# --- source enumeration ------------------------------------------------------

def fst_overlay_tiles(fst_path: Path) -> dict:
    """basename(lower) -> raw TGA bytes for every 64overlays entry."""
    out = {}
    for off, comp, real, name, is_lz, data in fstmod.fst_entries(str(fst_path)):
        n = name.lower().replace("\\", "/")
        if "/64overlays/" not in n or not n.endswith(".tga"):
            continue
        raw = data[off:off + comp]
        if comp == real:
            payload = raw
        else:
            # This textures.fst carries zlib streams; fall back to the classic
            # LZW port for older archives.
            try:
                payload = zlib.decompress(raw)
            except zlib.error:
                payload = fstmod.lzw_decomp(raw, real)
        out[os.path.basename(n)] = payload
    return out


def gather_sources(deploy: Path, families) -> dict:
    """basename(lower) -> TGA bytes; loose data/textures/64Overlays wins."""
    fam_re = re.compile(
        r"^(" + "|".join(re.escape(f) for f in families) + r")[0-9]+\.tga$")
    tiles = {}
    for base, buf in fst_overlay_tiles(deploy / "textures.fst").items():
        if fam_re.match(base):
            tiles[base] = buf
    loose_dir = deploy / "data" / "textures" / "64Overlays"
    if loose_dir.is_dir():
        for p in sorted(loose_dir.glob("*.tga")):
            base = p.name.lower()
            if fam_re.match(base):
                tiles[base] = p.read_bytes()
    return tiles


# --- colorkey-safe upscale ---------------------------------------------------

def chroma_magenta_mask(px: np.ndarray) -> np.ndarray:
    """The engine's transition-bake colorkey test (terrtxm.cpp:459)."""
    b = px[:, :, 0].astype(np.int32)
    g = px[:, :, 1].astype(np.int32)
    r = px[:, :, 2].astype(np.int32)
    return (r > 150) & (b > 150) & (r > g + 40) & (b > g + 40)


def inpaint_masked(rgb: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """Fill mask=True pixels with the nearest unmasked color (iterative
    4-neighbour dilation; tiles are 64px so a few passes converge)."""
    out = rgb.copy()
    todo = mask.copy()
    for _ in range(128):
        if not todo.any():
            break
        filled = np.zeros_like(todo)
        acc = np.zeros(rgb.shape, dtype=np.float32)
        cnt = np.zeros(rgb.shape[:2], dtype=np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            src_ok = np.roll(~todo, (dy, dx), axis=(0, 1))
            src_val = np.roll(out, (dy, dx), axis=(0, 1)).astype(np.float32)
            take = todo & src_ok
            acc[take] += src_val[take]
            cnt[take] += 1.0
            filled |= take
        got = filled & (cnt > 0)
        out[got] = (acc[got] / cnt[got, None]).astype(np.uint8)
        todo &= ~got
    return out


class Upscaler:
    def __init__(self, model_path: str):
        import torch
        from spandrel import ModelLoader
        self.torch = torch
        torch.set_num_threads(max(1, (os.cpu_count() or 8) - 2))
        self.model = ModelLoader().load_from_file(model_path)
        self.model.model.train(False)   # inference mode (== module eval mode)
        self.scale = self.model.scale

    def upscale_rgb(self, rgb: np.ndarray) -> np.ndarray:
        """HxWx3 uint8 RGB -> (H*scale)x(W*scale)x3 uint8."""
        t = self.torch.from_numpy(rgb.astype(np.float32) / 255.0)
        t = t.permute(2, 0, 1).unsqueeze(0)
        with self.torch.no_grad():
            y = self.model.model(t)
        y = y.squeeze(0).permute(1, 2, 0).clamp_(0.0, 1.0).numpy()
        return (y * 255.0 + 0.5).astype(np.uint8)


def upscale_tile(buf: bytes, up: Upscaler, edge: int):
    """One tile: colorkey-safe 4x AI upscale. Returns (desc, HxWx4 BGRA)."""
    from PIL import Image

    desc, px = tga_read(buf)          # BGRA, file row order
    h, w = px.shape[:2]
    if (w, h) != (64, 64):
        raise ValueError(f"expected 64x64 source tile, got {w}x{h}")
    scale = edge // 64

    mask64 = chroma_magenta_mask(px)
    rgb64 = px[:, :, [2, 1, 0]]       # -> RGB for the model
    if mask64.any():
        rgb64 = inpaint_masked(rgb64, mask64)

    rgb_up = up.upscale_rgb(rgb64)
    if up.scale != scale:
        rgb_up = np.asarray(Image.fromarray(rgb_up).resize(
            (edge, edge), Image.LANCZOS))

    # Alpha (road-shape mask): faithful Lanczos, no AI.
    a_up = np.asarray(Image.fromarray(px[:, :, 3]).resize(
        (edge, edge), Image.LANCZOS))

    out = np.empty((edge, edge, 4), dtype=np.uint8)
    out[:, :, 0] = rgb_up[:, :, 2]    # B
    out[:, :, 1] = rgb_up[:, :, 1]    # G
    out[:, :, 2] = rgb_up[:, :, 0]    # R
    out[:, :, 3] = a_up

    if mask64.any():
        # Restore EXACT colorkey where the (soft-thresholded) mask lands.
        m_up = np.asarray(Image.fromarray(
            (mask64 * 255).astype(np.uint8)).resize(
                (edge, edge), Image.BILINEAR)) >= 128
        out[m_up, 0] = 255            # B
        out[m_up, 1] = 0              # G
        out[m_up, 2] = 255            # R
        out[m_up, 3] = 0

    # Anti-hallucination clamp: no NON-colorkey output pixel may pass the
    # engine chroma test (the bake path would zero it).
    bad = chroma_magenta_mask(out)
    if mask64.any():
        m_up = np.asarray(Image.fromarray(
            (mask64 * 255).astype(np.uint8)).resize(
                (edge, edge), Image.BILINEAR)) >= 128
        bad &= ~m_up
    if bad.any():
        rb_min = np.minimum(out[:, :, 0], out[:, :, 2]).astype(np.int32)
        g_fix = np.clip(rb_min - 40, 0, 255).astype(np.uint8)
        gch = out[:, :, 1]
        gch[bad] = np.maximum(gch[bad], g_fix[bad])
        out[:, :, 1] = gch

    return desc, out


# --- contact sheet -----------------------------------------------------------

def contact_sheet(pairs, path: Path) -> None:
    """pairs: list of (name, px64 BGRA, pxN BGRA). 64 (nearest-zoomed) vs
    hi-res side by side, one row per tile."""
    from PIL import Image, ImageDraw
    if not pairs:
        return
    edge = pairs[0][2].shape[0]
    cell = edge
    pad, label_h = 8, 14
    rows = len(pairs)
    img = Image.new("RGB", (2 * cell + 3 * pad, rows * (cell + label_h + pad) + pad),
                    (24, 24, 24))
    draw = ImageDraw.Draw(img)
    y = pad
    for name, p64, pN in pairs:
        left = Image.fromarray(p64[:, :, [2, 1, 0]]).resize(
            (cell, cell), Image.NEAREST)
        right = Image.fromarray(pN[:, :, [2, 1, 0]])
        img.paste(left, (pad, y + label_h))
        img.paste(right, (2 * pad + cell, y + label_h))
        draw.text((pad, y), f"{name}  64px (nearest x{cell//64})", fill=(230, 230, 230))
        draw.text((2 * pad + cell, y), f"{edge}px UltraSharpV2", fill=(230, 230, 230))
        y += cell + label_h + pad
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


# --- main --------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="OVERLAY-TILE-HIRES-1 tile cook")
    ap.add_argument("--deploy", required=True,
                    help="deploy dir holding textures.fst + data/textures/64Overlays")
    ap.add_argument("--out-dir", required=True,
                    help="output folder (install as data/textures/<edge>Overlays)")
    ap.add_argument("--edge", type=int, default=256,
                    help="output tile edge (pow2, default 256)")
    ap.add_argument("--families", default=",".join(DEFAULT_FAMILIES))
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--limit", type=int, default=0, help="cook at most N tiles (testing)")
    ap.add_argument("--contact-sheet", default="",
                    help="write a 64-vs-hires side-by-side PNG for sample tiles")
    ap.add_argument("--sheet-tiles", default="runway0006.tga,pavedroad0003.tga,"
                    "dirtroad0001.tga,x_dirt_paved0000.tga",
                    help="comma list of tile basenames for the contact sheet")
    args = ap.parse_args(argv)

    if args.edge & (args.edge - 1) or not (128 <= args.edge <= 1024):
        ap.error("--edge must be a power of two in [128,1024]")

    deploy = Path(args.deploy)
    out_dir = Path(args.out_dir)
    families = [f.strip() for f in args.families.split(",") if f.strip()]

    tiles = gather_sources(deploy, families)
    names = sorted(tiles)
    if args.limit:
        names = names[:args.limit]
    print(f"[overlay_upscale] tiles={len(names)} edge={args.edge} "
          f"model={os.path.basename(args.model)}")

    up = Upscaler(args.model)
    sheet_want = {s.strip().lower() for s in args.sheet_tiles.split(",")}
    sheet_pairs = []
    t0 = time.time()
    n_magenta = 0
    for i, base in enumerate(names):
        desc, out_px = upscale_tile(tiles[base], up, args.edge)
        _, src_px = tga_read(tiles[base])
        if chroma_magenta_mask(src_px).any():
            n_magenta += 1
        tga_write(out_dir / base, out_px, desc)
        if base in sheet_want:
            sheet_pairs.append((base, src_px, out_px))
        if (i + 1) % 25 == 0 or i + 1 == len(names):
            dt = time.time() - t0
            print(f"  {i+1}/{len(names)}  {dt:.0f}s  ({dt/(i+1):.2f}s/tile)")

    # Output validation: every cooked tile must be free of NON-colorkey
    # chroma-magenta pixels and byte-decodable at the requested edge.
    bad = []
    for base in names:
        buf = (out_dir / base).read_bytes()
        desc, px = tga_read(buf)
        if px.shape[:2] != (args.edge, args.edge):
            bad.append((base, "size"))
        exact_key = (px[:, :, 0] == 255) & (px[:, :, 1] == 0) & (px[:, :, 2] == 255)
        if (chroma_magenta_mask(px) & ~exact_key).any():
            bad.append((base, "chroma"))
    if bad:
        print("[overlay_upscale] VALIDATION FAILED:", bad[:10])
        return 1
    print(f"[overlay_upscale] OK: {len(names)} tiles -> {out_dir} "
          f"(src tiles with colorkey px: {n_magenta})")

    if args.contact_sheet and sheet_pairs:
        contact_sheet(sheet_pairs, Path(args.contact_sheet))
        print(f"[overlay_upscale] contact sheet -> {args.contact_sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
