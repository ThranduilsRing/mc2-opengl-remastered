#!/usr/bin/env python3
"""Split the stock pilot portrait sheet into one PNG per pilot.

Source: data/art/mcui_high2.tga (the highest-resolution stock sheet; low2/med2
carry the same portraits smaller). Grid: 7 columns of 33x46 cells; cells 0-27 are
the portraits plus the helmet (26) and the dead-pilot skull (27). Later cells hold
rank and eject glyphs that other widgets draw from their own sheets.

Output: data/art/pilots/<NN>.png, NN = PictureIndex zero-padded to two digits.
Deterministic: fixed crop, no resampling, PNG without metadata.

  py -3 tools/split_pilot_portraits.py --sheet <mcui_high2.tga> --out <repo root>
  py -3 tools/split_pilot_portraits.py --fst <art.fst> --out <repo root>
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

COLS, CELL_W, CELL_H, COUNT = 7, 33, 46, 28
SHEET_MEMBER = "data/art/mcui_high2.tga"


def sheet_from_fst(fst: Path) -> bytes:
    data = fst.read_bytes()
    _ver, n = struct.unpack_from("<II", data, 0)
    for i in range(n):
        off, size, real, _h = struct.unpack_from("<IIII", data, 8 + i * 266)
        name = data[8 + i * 266 + 16:8 + i * 266 + 266].split(b"\0")[0].decode("latin-1")
        if name.replace("\\", "/").lower() == SHEET_MEMBER:
            raw = data[off:off + size]
            return raw if size == real else zlib.decompress(raw)
    raise SystemExit(f"{SHEET_MEMBER} not found in {fst}")


def split(sheet: Image.Image):
    sheet = sheet.convert("RGBA")
    for index in range(COUNT):
        x, y = index % COLS * CELL_W, index // COLS * CELL_H
        yield index, sheet.crop((x, y, x + CELL_W, y + CELL_H))


def main() -> int:
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--sheet", type=Path)
    src.add_argument("--fst", type=Path)
    ap.add_argument("--out", type=Path, required=True, help="root that receives data/art/pilots/")
    args = ap.parse_args()
    if args.fst:
        import io
        sheet = Image.open(io.BytesIO(sheet_from_fst(args.fst)))
    else:
        sheet = Image.open(args.sheet)
    dest = args.out / "data" / "art" / "pilots"
    dest.mkdir(parents=True, exist_ok=True)
    for index, tile in split(sheet):
        tile.save(dest / f"{index:02d}.png", optimize=False)
    print(f"[split_pilot_portraits] wrote {COUNT} portraits to {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
