#!/usr/bin/env python3
"""UI-ART-RECONSTRUCT-1 — offline reconstruction of legacy stitched UI panels.

The 1999 MC2 UI panels are STITCHED at runtime from many small UV-crops of a few
128x128 shared atlases (MCUI_Universal{1,2,3}.png): each `[<Panel>Static N]` block
in the legacy buttonLayout*.fit names an atlas + a UV rect (UNormal/VNormal +
UWidth/VHeight) + a screen rect (XLocation/YLocation/Width/Height), blitted 1:1
onto the 800x600 authored canvas. They tiled because of period texture-size limits.

We have no such limit — so this tool composes the tiles OFFLINE into ONE flattened
RGBA PNG per panel (the exact pixels the game assembles), which the ImGui port can
sample as a single texture instead of re-stitching at runtime. Reusable for every
menu recreation: point --panel at any `<Prefix>Window` block.

Engine-parity notes:
  * PNG magenta key (PNG-MAGENTA-KEY-1): exact FF00FF -> transparent black at load.
  * UV origin is top-left (VNormal counts down from the atlas top), Width==UWidth
    for stock panels (1:1 blit, no scale); we honor a scale if they ever differ.

Usage:
  py -3 tools/ui_art_reconstruct.py \
     --fit <install>/data/defs/ui/legacy/game/buttonLayout800.fit \
     --art-dir <install>/data/art/gui \
     --panel Pause \
     --out dev_out/pause_frame.png
"""
from __future__ import annotations

import argparse
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: py -3 -m pip install Pillow")

MAGENTA = (255, 0, 255)


def read_fit(path: str) -> str:
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        return f.read()


def parse_block(text: str, block: str) -> dict | None:
    """Return {key: value} for a `[block]` up to the next `[`. Values are the
    raw token after '=' (int-coerced when numeric, hex when 0x..)."""
    m = re.search(r"^\[" + re.escape(block) + r"\][^\n]*\n(.*?)(?=^\[|\Z)",
                  text, re.MULTILINE | re.DOTALL | re.IGNORECASE)
    if not m:
        return None
    out: dict = {}
    for line in m.group(1).splitlines():
        km = re.match(r"\s*(?:st|l|f|b|c|ul)\s+(\w+)\s*=\s*(.+?)\s*(?://.*)?$", line)
        if not km:
            continue
        key, raw = km.group(1), km.group(2).strip()
        if raw.startswith('"') and raw.endswith('"'):
            out[key] = raw[1:-1]
        elif raw.lower().startswith("0x"):
            out[key] = int(raw, 16)
        else:
            try:
                out[key] = int(raw)
            except ValueError:
                out[key] = raw
    return out


def load_atlas(art_dir: str, name: str, cache: dict) -> Image.Image:
    if name in cache:
        return cache[name]
    # FileName in the fit has no extension; stock ships .png (magenta-keyed).
    for ext in (".png", ".tga", ".PNG", ".TGA"):
        p = os.path.join(art_dir, name + ext)
        if os.path.isfile(p):
            im = Image.open(p).convert("RGBA")
            # PNG magenta key -> transparent (engine PNG-MAGENTA-KEY-1 parity).
            px = im.getdata()
            im.putdata([(0, 0, 0, 0) if (r, g, b) == MAGENTA else (r, g, b, a)
                        for (r, g, b, a) in px])
            cache[name] = im
            return im
    raise FileNotFoundError(f"atlas '{name}' not found in {art_dir}")


def reconstruct(fit_path: str, art_dir: str, panel: str, default_atlas: str):
    text = read_fit(fit_path)
    hdr = parse_block(text, panel + "Window") or {}
    n_static = int(hdr.get("StaticCount", 0))
    n_button = int(hdr.get("ButtonCount", 0))

    # Many panels (Obj, MechBay, ...) enumerate `<Panel>Static N` blocks with no
    # `<Panel>Window` count header. Auto-discover: count consecutive blocks from 0.
    def count_blocks(kind: str) -> int:
        i = 0
        while re.search(r"^\[" + re.escape(f"{panel}{kind}{i}") + r"\]",
                        text, re.MULTILINE | re.IGNORECASE):
            i += 1
        return i
    if n_static == 0:
        n_static = count_blocks("Static")
    if n_button == 0:
        n_button = count_blocks("Button")
    if n_static == 0 and n_button == 0:
        sys.exit(f"no [{panel}Window]/[{panel}Static N]/[{panel}Button N] "
                 f"blocks in {fit_path}")

    canvas = Image.new("RGBA", (800, 600), (0, 0, 0, 0))
    cache: dict = {}
    tiles = 0
    minx, miny, maxx, maxy = 800, 600, 0, 0
    missing: set = set()

    def blit(kind: str, count: int):
        nonlocal tiles, minx, miny, maxx, maxy
        for i in range(count):
            b = parse_block(text, f"{panel}{kind}{i}")
            if not b or "XLocation" not in b:
                continue
            atlas = str(b.get("FileName", default_atlas))
            x, y = int(b["XLocation"]), int(b["YLocation"])
            w, h = int(b["Width"]), int(b["Height"])
            u, v = int(b.get("UNormal", 0)), int(b.get("VNormal", 0))
            uw, vh = int(b.get("UWidth", w)), int(b.get("VHeight", h))
            try:
                im = load_atlas(art_dir, atlas, cache)
            except FileNotFoundError:
                # Atlas not under art_dir (e.g. packed in an FST archive) — record
                # it and skip; the tool reports the full missing set so the user
                # knows exactly which art to extract, instead of crashing on the
                # first one.
                missing.add(atlas)
                continue
            crop = im.crop((u, v, u + uw, v + vh))
            if (uw, vh) != (w, h):  # honor scale if a panel differs (stock: 1:1)
                crop = crop.resize((w, h), Image.NEAREST)
            canvas.alpha_composite(crop, (x, y))
            tiles += 1
            minx, miny = min(minx, x), min(miny, y)
            maxx, maxy = max(maxx, x + w), max(maxy, y + h)

    # Statics = the panel frame/chrome (the stitched pieces). Buttons = NORMAL
    # (up) state, so the baked frame shows the resting panel; the ImGui port
    # renders live button states on top.
    blit("Static", n_static)
    blit("Button", n_button)

    bbox = (minx, miny, maxx, maxy) if tiles else (0, 0, 800, 600)
    return canvas, bbox, tiles, n_static, n_button, sorted(missing)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fit", required=True)
    ap.add_argument("--art-dir", required=True)
    ap.add_argument("--panel", required=True, help="block prefix, e.g. Pause / Info")
    ap.add_argument("--default-atlas", default="MCUI_Universal1")
    ap.add_argument("--out", required=True)
    ap.add_argument("--crop", action="store_true",
                    help="crop the output PNG to the used bbox (prints origin)")
    args = ap.parse_args()

    canvas, bbox, tiles, ns, nb, missing = reconstruct(
        args.fit, args.art_dir, args.panel, args.default_atlas)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    out = canvas
    if args.crop:
        out = canvas.crop(bbox)
        print(f"[ui-art] bbox origin=({bbox[0]},{bbox[1]}) "
              f"size=({bbox[2]-bbox[0]}x{bbox[3]-bbox[1]}) in 800x600 authored space")
    out.save(args.out)
    print(f"[ui-art] {args.panel}: composed {tiles} tiles "
          f"({ns} statics + {nb} buttons) -> {args.out}")
    if missing:
        print(f"[ui-art] WARNING: {len(missing)} atlas(es) not under --art-dir "
              f"(likely packed in an FST — extract them): {', '.join(missing)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
