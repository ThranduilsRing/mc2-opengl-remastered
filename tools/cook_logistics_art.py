"""Cook briefing maps and named bay/HUD icons from an install (Pillow + numpy).

  py -3 tools/cook_logistics_art.py --source <install> --out <worktree> --proof <dir>

Use --source <mod-install> --base <stock-install> to cook a mod's own data.
Inputs are read only. Outputs are optional loose overrides; original PAKs/atlases
are never replaced. Maps use the editor's north-up, full N*128 world-unit extent.
"""
from __future__ import annotations

import argparse
import csv
import io
import math
from pathlib import Path
import re
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.mc2x_import.fst import fst_entries, decompress_entry, lzw_decomp
from tools.terrain_beautify.mission_terrain_analyzer import locate_mapdata, extract_layers


class Assets:
    """Loose first, then the source's FSTs, then optional stock fallback."""
    def __init__(self, root: Path, base: Assets | None = None):
        self.root, self.base = root, base
        self.entries = {}
        for name in ("art", "mission", "textures", "misc", "tgl"):
            archive = root / f"{name}.fst"
            if archive.is_file():
                for off, comp, real, key, lz, data in fst_entries(archive):
                    self.entries[key.replace('\\', '/').lower()] = (off, comp, real, lz, data)

    def keys(self, prefix, suffix):
        keys = {k for k in self.entries if k.startswith(prefix) and k.endswith(suffix)}
        if self.base:
            keys.update(self.base.keys(prefix, suffix))
        folder = self.root / prefix
        if folder.is_dir():
            keys.update(p.relative_to(self.root).as_posix().lower() for p in folder.glob(f"*{suffix}"))
        return sorted(keys)

    def read(self, key):
        key = key.replace('\\', '/').lower()
        loose = self.root / key
        if loose.is_file():
            return loose.read_bytes()
        if key in self.entries:
            return decompress_entry(*self.entries[key])
        if self.base:
            return self.base.read(key)
        raise FileNotFoundError(key)

    def image(self, key):
        return Image.open(io.BytesIO(self.read(key))).convert("RGBA")


def packets(data):
    """PacketFile offsets, including LZW and zlib packet compression."""
    if len(data) < 12 or struct.unpack_from('<I', data)[0] != 0xFEEDFACE:
        raise ValueError("not an MC2 PacketFile")
    count = struct.unpack_from('<I', data, 4)[0] // 4 - 2
    if count < 1 or 8 + count * 4 > len(data):
        raise ValueError("bad packet table")
    entries = struct.unpack_from(f'<{count}I', data, 8)
    result = []
    for i, entry in enumerate(entries):
        kind, start = entry >> 29, entry & 0x1fffffff
        end = entries[i + 1] & 0x1fffffff if i + 1 < count else len(data)
        if not 8 + count * 4 <= start <= end <= len(data):
            raise ValueError("bad packet bounds")
        payload = data[start:end]
        if kind in (2, 4) and len(payload) >= 4:
            size = struct.unpack_from('<I', payload)[0]
            if size > 64*1024*1024:
                raise ValueError('packet exceeds 64 MB')
            raw = payload[4:]
            is_zlib = len(raw) >= 2 and raw[0] & 15 == 8 and (raw[0]*256+raw[1]) % 31 == 0
            payload = zlib.decompress(raw) if kind == 4 or is_zlib else lzw_decomp(raw, size)
            if len(payload) != size:
                raise ValueError('bad decompressed packet size')
        elif kind == 7:
            payload = b""
        elif kind != 0:
            payload = b""
        result.append((i, 0, payload))
    return result


def fit_value(text, block, key, default=""):
    text = re.sub(r'(?m)//[^\r\n]*', '', text)
    section = re.search(r'(?ims)^\s*\[' + re.escape(block) + r'\]\s*$(.*?)(?=^\s*\[|\Z)', text)
    if section:
        found = re.search(r'(?im)^\s*\w+\s+' + re.escape(key) + r'\s*=\s*([^\r\n;]+)', section[1])
        if found:
            return found[1].strip().strip('"')
    return default


def transparent_border(image, hud=False):
    pixels = np.array(image.convert("RGBA"))
    # The legacy bay is black keyed. HUD palette colours must stay exact.
    if not hud:
        pixels[np.sum(pixels[:, :, :3].astype(int), axis=2) <= 16] = 0
        pixels[(pixels[:, :, 0] == 255) & (pixels[:, :, 1] == 0) & (pixels[:, :, 2] == 255)] = 0
        # Stock unused slots are solid pink (226,0,127), not TGA magenta.
        if np.all(pixels == pixels[0, 0]) and pixels[0, 0, 0] > 200 and pixels[0, 0, 1] < 10 and pixels[0, 0, 2] > 100:
            pixels[:] = 0
    pixels[pixels[:, :, 3] == 0] = 0
    return ImageOps.expand(Image.fromarray(pixels), border=1, fill=(0, 0, 0, 0))


def split_icons(assets, out):
    layout = assets.read("data/art/mcl_gn_availablemechentry.fit").decode('latin1')
    cw = int(fit_value(layout, "Static1", "Width", "25"))
    ch = int(fit_value(layout, "Static1", "Height", "30"))
    sheet_name = fit_value(layout, "Static1", "FileName", "mcui_gn_mechicons")
    sheet_path = "data/art/" + sheet_name.lower()
    if not sheet_path.endswith(('.tga', '.png')):
        sheet_path += '.tga'
    atlas = assets.image(sheet_path)
    try:
        sizes = csv.reader(io.StringIO(assets.read('data/art/asset_sizes.csv').decode('latin1'), newline=None))
        for row in sizes:
            if len(row) >= 3 and 'data/' + row[0].strip().lower() == sheet_path:
                cw = round(cw*atlas.width/int(row[1]))
                ch = round(ch*atlas.height/int(row[2]))
                break
    except FileNotFoundError:
        pass
    sheets = {}
    for name, width, height in (("mcui_low4", 25, 30), ("mcui_med4", 32, 38), ("mcui_high7", 40, 48)):
        try:
            sheets[name] = (assets.image(f"data/art/{name}.tga"), width, height)
        except FileNotFoundError:
            pass
    written = []
    for key in assets.keys("data/objects/", ".csv"):
        rows = csv.reader(io.StringIO(assets.read(key).decode('latin1'), newline=None))
        # Some mod CSVs use CRCRLF. Find the semantic row rather than counting blanks.
        row = next((r for r in rows if len(r) >= 2 and r[0].strip().lower() in ('iconindex', 'mechparts')), None)
        if row is None:
            continue
        try:
            index = int(row[1])
        except ValueError:
            continue
        name = Path(key).stem
        cols = atlas.width // cw
        if index < 0:
            continue
        if cols > 0 and (index // cols + 1)*ch <= atlas.height:
            x, y = index % cols * cw, index // cols * ch
            icon = transparent_border(atlas.crop((x, y, x + cw, y + ch)))
            if icon.getbbox():
                dest = out / f"data/art/mechicons/{name}.png"
                dest.parent.mkdir(parents=True, exist_ok=True)
                icon.save(dest)
                written.append((name, icon))
        for sheet, (image, w, h) in sheets.items():
            # HUD height, not total width, defines scale (mods add columns).
            scale = image.height / (h * 2)
            if (index + 1) * w * scale > image.width:
                continue
            for row, side in enumerate(("front", "back")):
                tile = image.crop((round(index*w*scale), round(row*h*scale),
                                   round((index+1)*w*scale), round((row+1)*h*scale)))
                dest = out / f"data/art/mechicons/hud/{sheet}/{side}/{name}.png"
                dest.parent.mkdir(parents=True, exist_ok=True)
                hud_icon = transparent_border(tile, hud=True)
                hud_icon.save(dest)
                written.append((f'{name}:{sheet.removeprefix("mcui_")}:{side[0]}', hud_icon))
    return written


def overlay_table(assets):
    text = assets.read("data/textures/textures.fit").decode('latin1')
    base = int(fit_value(text, "Main", "MaxTerrainTypes", "255"))
    result = {}
    for i in range(256):
        name = fit_value(text, f"OverlayType{i}", "OverlayName")
        if not name:
            break
        count = int(fit_value(text, f"OverlayType{i}", "NumOverlays", "0"))
        for j in range(count):
            result[base + j] = f"data/textures/64overlays/{name.lower()}{j:04d}.tga"
        base += count
    return result


def stamp_objects(assets, image, data, n):
    # EditorObjectMgr saves the catalog FitID followed by XYZ/heading and five ints.
    rows = csv.reader(io.StringIO(assets.read('data/art/buildings.csv').decode('latin1'), newline=None))
    catalog = {}
    for row in rows:
        if len(row) >= 10 and row[4].strip().isdigit():
            catalog[int(row[4])] = row
    draw = ImageDraw.Draw(image)
    for _, _, payload in data:
        if len(payload) < 4:
            continue
        count = struct.unpack_from('<I', payload)[0]
        if not count or len(payload) != 4 + count*40:
            continue
        for i in range(count):
            ident, x, y = struct.unpack_from('<iff', payload, 4+i*40)
            row = catalog.get(ident)
            if row is None:
                continue
            # Same full-map extent as the editor colormap, no recentering.
            px, py = (x/(n*128)+.5)*image.width, (.5-y/(n*128))*image.height
            if not 0 <= px < image.width or not 0 <= py < image.height:
                continue
            color = int(row[8].strip() or '0', 0)
            if color:
                radius = max(1, image.width/(n*3))
                rgb = ((color >> 16) & 255, (color >> 8) & 255, color & 255)
                draw.rectangle((px-radius, py-radius, px+radius, py+radius), fill=rgb)
            if row[9].strip().lower() not in ('', 'none', '0'):
                try:
                    tile = assets.image('data/art/' + row[9].strip().lower() + '.tga')
                    w, h = max(1, round(tile.width*image.width/(n*3))), max(1, round(tile.height*image.height/(n*3)))
                    tile = tile.resize((w, h), Image.Resampling.LANCZOS)
                    image.paste(tile, (round(px-w/2), round(py-h/2)), tile)
                except FileNotFoundError:
                    pass
        break


def water_mask(assets, mission, elev, level, size):
    """Sample the corner-pinned height grid over the editor's full N*128 extent."""
    n = elev.shape[0]
    try:
        raw = assets.read(f'data/missions/{mission}.beauty/visual_height_4x.r32')
    except FileNotFoundError:
        raw = None
    if raw is not None:
        side = (n - 1) * 4 + 1
        if len(raw) != side * side * 4:
            raise ValueError('invalid visual height dimensions')
        elev = np.frombuffer(raw, dtype='<f4').reshape(side, side)
        if not np.isfinite(elev).all():
            raise ValueError('nonfinite visual heights')
    # Height vertices start at the northwest corner; the last cell clamps.
    coords = np.minimum((np.arange(size) + .5) * n / size, n - 1)
    coords *= (elev.shape[0] - 1) / (n - 1)
    lo = coords.astype(int)
    hi = np.minimum(lo + 1, elev.shape[0] - 1)
    frac = coords - lo
    rows = elev[lo, :] * (1 - frac[:, None]) + elev[hi, :] * frac[:, None]
    heights = rows[:, lo] * (1 - frac) + rows[:, hi] * frac
    dy, dx = np.gradient(heights)
    # One output pixel of coverage across the contour, including diagonal coasts.
    width = np.maximum(np.hypot(dx, dy), 1e-6)
    coverage = np.clip(.5 + (level - heights) / width, 0, 1)
    coverage[(dx == 0) & (dy == 0)] = heights[(dx == 0) & (dy == 0)] <= level
    return Image.fromarray(np.rint(coverage * 255).astype('uint8'))


def cook_map(assets, mission, size, overlays):
    data = packets(assets.read(f"data/missions/{mission}.pak"))
    md = locate_mapdata(data)
    if not md:
        raise ValueError("no terrain vertices")
    _, n, blocks = md
    fit = assets.read(f"data/missions/{mission}.fit").decode('latin1')
    layers = extract_layers(n, blocks, float(fit_value(fit, "Water", "Elevation", "0")))
    color_name = fit_value(fit, "ColorMap", "ColorMapName", mission)
    image = None
    for suffix in (".burnin.jpg", ".burnin.tga", ".tga", ".jpg"):
        try:
            image = assets.image(f"data/textures/{color_name}{suffix}")
            break
        except FileNotFoundError:
            pass
    if image is None:
        raise ValueError("no colormap (stock preview retained)")
    image = image.resize((size, size), Image.Resampling.BILINEAR).convert("RGB")
    # Same N*MAPCELL_DIM extent as EditorData::drawTacMap; no crop or rotation.
    water = water_mask(assets, mission, layers['elev'], layers['water_elev'], size)
    concrete = np.isin(layers['terrtype'], [10, 13, 14, 15, 16, 17, 18, 19, 20]) & ~layers['water']
    image.paste((147, 144, 131), mask=Image.fromarray(concrete.astype('uint8') * 255).resize(image.size, Image.Resampling.NEAREST))
    image.paste((36, 99, 132), mask=water)
    tex = np.frombuffer(blocks, dtype=np.dtype({'names':['tex'], 'formats':['<u4'], 'offsets':[16], 'itemsize':32}))['tex'].reshape(n, n) >> 16
    # Filter connected roads as one image: separate cell resizes clamp at each
    # tile edge and round their widths differently, leaving a faint square grid.
    roads = Image.new('RGBA', (n * 64, n * 64))
    tiles = {}
    for r, c in zip(*np.where((tex != 0) & (tex != 65535))):
        path = overlays.get(int(tex[r, c]))
        if not path:
            raise ValueError(f"unknown overlay handle {tex[r,c]}")
        if path not in tiles:
            tile = np.array(assets.image(path))
            tile[(tile[:, :, 0] == 255) & (tile[:, :, 1] == 0) & (tile[:, :, 2] == 255)] = 0
            if tile.shape != (64, 64, 4):
                raise ValueError(f'invalid 64overlays tile: {path}')
            tiles[path] = Image.fromarray(tile)
        roads.paste(tiles[path], (c * 64, r * 64))
    roads = roads.resize(image.size, Image.Resampling.BILINEAR)
    image.paste(roads, (0, 0), roads)
    stamp_objects(assets, image, data, n)
    return image


def contact_sheet(items, path, cell=(180, 200), columns=6):
    if not items:
        raise ValueError(f"no images for {path.name}")
    sheet = Image.new('RGB', (columns*cell[0], math.ceil(len(items)/columns)*cell[1]), (24, 28, 34))
    draw = ImageDraw.Draw(sheet)
    for i, (name, image) in enumerate(items):
        tile = image.copy()
        tile.thumbnail((cell[0]-8, cell[1]-24), Image.Resampling.NEAREST if image.width < 100 else Image.Resampling.LANCZOS)
        x, y = i % columns * cell[0], i // columns * cell[1]
        sheet.paste(tile, (x+(cell[0]-tile.width)//2, y), tile if tile.mode == 'RGBA' else None)
        draw.text((x+4, y+cell[1]-20), name, fill='white')
    sheet.save(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--base', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--size', type=int, choices=(512, 1024), default=1024)
    args = parser.parse_args(argv)
    if args.source.resolve() == args.out.resolve():
        parser.error('source and output must differ')
    assets = Assets(args.source, Assets(args.base) if args.base else None)
    args.proof.mkdir(parents=True, exist_ok=True)
    icons = split_icons(assets, args.out)
    maps, skipped = [], []
    overlays = overlay_table(assets)
    for key in assets.keys('data/missions/', '.pak'):
        mission = Path(key).stem
        try:
            image = cook_map(assets, mission, args.size, overlays)
            dest = args.out / f'data/art/missionmaps/{mission}.png'
            dest.parent.mkdir(parents=True, exist_ok=True)
            image.save(dest)
            maps.append((mission, image))
        except (FileNotFoundError, ValueError) as error:
            skipped.append(f'{mission}: {error}')
    contact_sheet(maps, args.proof/'minimaps.png')
    contact_sheet(icons, args.proof/'icons.png', cell=(180, 100), columns=8)
    chassis = sum(':' not in name for name, _ in icons)
    (args.proof/'cook.txt').write_text(f'maps={len(maps)} chassis={chassis} images={len(icons)}\n' + '\n'.join(skipped) + '\n')
    print(f'logistics-art: maps={len(maps)} chassis={chassis} images={len(icons)} skipped={len(skipped)}; {args.proof}/cook.txt')
    required = {Path(k).stem for k in assets.keys('data/missions/', '.pak')
                if re.fullmatch(r'mc2_\d{2}', Path(k).stem)}
    missing = sorted(required - {name for name, _ in maps})
    if missing:
        print('ERROR: stock mission maps not cooked: ' + ', '.join(missing))
    return 0 if maps and icons and not missing else 1


if __name__ == '__main__':
    raise SystemExit(main())
