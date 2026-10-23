"""Fast Blender-free preview of a built kit asset (painter's-algorithm flat shading).

`run.py preview <asset>` -> arch-proof/<asset>_preview.png: pristine from 5 sides + the 5 states in iso.
Only a development aid; the real renders and bakes are the Blender passes.
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import archetypes
import drafts
import geo

STATES = ('pristine', 'worn', 'abandoned', 'partial', 'destroyed')
DIRS = {'front': ((0, 1, 0), (0, 0, 1)), 'left': ((1, 0, 0), (0, 0, 1)), 'rear': ((0, -1, 0), (0, 0, 1)),
        'right': ((-1, 0, 0), (0, 0, 1)), 'top': ((0, 0, -1), (0, 1, 0)), 'iso': ((.55, .8, -.5), (0, 0, 1))}


def _srgb(c):
    c = np.clip(np.asarray(c, float), 0, 1)
    return np.where(c <= .0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - .055)


def render(k, view='iso', px=360, size=None):
    d, up = DIRS[view]
    d = np.array(d, float) / np.linalg.norm(d)
    r = np.cross(d, up)
    r /= np.linalg.norm(r)
    u = np.cross(r, d)
    pal = geo.LANG['palettes'][k.palette]
    tris = []
    for pc in k.pieces:
        col = archetypes.slot_colour(pc['m'], pal) or ((.7, .55, .2) if pc['m'].startswith('lamp') else (.05, .2, .05))
        base = _srgb(col)
        V = np.array(pc['v'], float)
        for f in pc['f']:
            for i in range(1, len(f) - 1):
                tris.append((V[[f[0], f[i], f[i + 1]]], base))
    if not tris:
        return Image.new('RGB', (px, px), (40, 44, 52))
    allp = np.concatenate([t[0] for t in tris])
    P = np.stack([allp @ r, allp @ u], 1)
    lo, hi = P.min(0), P.max(0)
    sc = (px * .9) / max(hi[0] - lo[0], hi[1] - lo[1], 1e-6)
    im = Image.new('RGB', (px, px), (46, 50, 60))
    dr = ImageDraw.Draw(im)
    light = np.array([-.4, -.5, .75])
    light /= np.linalg.norm(light)
    order = sorted(tris, key=lambda t: -float((t[0] @ d).mean()))
    for t, base in order:
        n = np.cross(t[1] - t[0], t[2] - t[0])
        ln = np.linalg.norm(n)
        if ln < 1e-12:
            continue
        n /= ln
        if n @ d > 0:
            n = -n
        shade = .45 + .55 * max(0.0, float(n @ light))
        xy = [((float(q @ r) - lo[0]) * sc + px * .05, px - ((float(q @ u) - lo[1]) * sc + px * .05)) for q in t]
        dr.polygon(xy, fill=tuple(int(255 * min(1, c * shade)) for c in base))
    return im


def sheet(name, out, px=300):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    geo.GAME_MESH = True
    try:
        ks = {s: drafts.build(name, s) for s in STATES}
    finally:
        geo.GAME_MESH = False
    row1 = [render(ks['pristine'], v, px) for v in ('front', 'left', 'rear', 'right', 'top')]
    row2 = [render(ks[s], 'iso', px) for s in STATES]
    im = Image.new('RGB', (px * 5, px * 2), (30, 30, 34))
    for i, a in enumerate(row1):
        im.paste(a, (i * px, 0))
    for i, a in enumerate(row2):
        im.paste(a, (i * px, px))
    path = out / f'{name}_preview.png'
    im.save(path)
    return path, {s: sum(sum(len(f) - 2 for f in p['f']) for p in k.pieces) for s, k in ks.items()}


def compare(name, out, px=300):
    """Stock (decoded mesh + its texture) vs remake (game carrier, flat shaded): the same five cameras, one row each."""
    from PIL import ImageOps
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    views = ('front', 'left', 'rear', 'right', 'top')
    stock_dir = Path('E:/mc2-ref/scifi-kit/studies/stock-views')
    geo.GAME_MESH = True
    try:
        k = drafts.build(name, 'pristine')
    finally:
        geo.GAME_MESH = False
    im = Image.new('RGB', (px * 5 + 90, px * 2 + 30), (30, 30, 34))
    dr = ImageDraw.Draw(im)
    dr.text((6, 8), f'{name}: stock (top row) vs remake game carrier (bottom row); front / left / rear / right / top', fill=(220, 220, 220))
    dr.text((6, 30 + px // 2), 'stock', fill=(220, 220, 220))
    dr.text((6, 30 + px + px // 2), 'remake', fill=(220, 220, 220))
    for i, v in enumerate(views):
        f = stock_dir / f'{name}_stock_{v}.png'
        if f.is_file():
            im.paste(ImageOps.pad(Image.open(f).convert('RGB'), (px - 4, px - 4), color=(46, 50, 60)), (90 + i * px + 2, 30 + 2))
        im.paste(render(k, v, px - 4), (90 + i * px + 2, 30 + px + 2))
    path = out / f'{name}_stock_vs_remake.png'
    im.save(path)
    return path
