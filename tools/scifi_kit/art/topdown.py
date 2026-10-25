"""Top-down orthographic plan of the stock mesh and the remake at the same scale, side by side (owner 2026-10-24:
"compare a top-down plan against the stock footprint before baking"). Height-shaded, metre grid, the stock footprint
outline drawn over the remake.

  py -3 tools/scifi_kit/art/topdown.py s_spotlightcontrol,l_spotlightcontrol [--out DIR]
Writes <out>/<name>_topdown.png (default: E:/mc2-ref/scifi-kit/art/intent-pass/<name>/) and prints the footprint
overlap (IoU of the covered plan cells above 0.5 m).
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import analyze

OUT = Path('E:/mc2-ref/scifi-kit/art/intent-pass')
PX = 8.0          # pixels per metre


def stock_tris(name, part=None):
    """Stock triangles in the authored frame; part='door' / 'static' keeps only the gate door node / everything else."""
    st = analyze.load_stock(name)
    V = st['V'].astype(float)
    c = np.array([(V[..., 0].min() + V[..., 0].max()) / 2, (V[..., 1].min() + V[..., 1].max()) / 2, V[..., 2].min()])
    V = V - c
    if part:
        keep = np.zeros(len(V), bool)
        for nm, a, b in st['parts']:
            isdoor = 'door' in nm.lower()
            if isdoor == (part == 'door'):
                keep[a:b] = True
        V = V[keep]
    return V


def remake_tris(name, skip_anim=None, raw=False):
    import geo
    import intent
    geo.GAME_MESH = True
    try:
        k = intent.build(name, raw=raw)
    finally:
        geo.GAME_MESH = False
    T = []
    for pc in k.pieces:
        if skip_anim and skip_anim in (pc.get('anim') or ''):
            continue
        v = np.asarray(pc['v'], float)
        for f in pc['f']:
            for j in range(1, len(f) - 1):
                T.append((v[f[0]], v[f[j]], v[f[j + 1]]))
    return np.array(T)


def raster(T, box, px=PX):
    x0, y0, x1, y1 = box
    W, H = int((x1 - x0) * px) + 1, int((y1 - y0) * px) + 1
    Z = np.full((H, W), -1.0)
    for t in T:
        P = np.c_[(t[:, 0] - x0) * px, (y1 - t[:, 1]) * px]
        lo = np.floor(P.min(0)).astype(int)
        hi = np.ceil(P.max(0)).astype(int)
        lo = np.maximum(lo, 0)
        hi = np.minimum(hi, [W - 1, H - 1])
        if (hi < lo).any():
            continue
        xs, ys = np.meshgrid(np.arange(lo[0], hi[0] + 1) + .5, np.arange(lo[1], hi[1] + 1) + .5)
        a, b, c = P
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-9:
            continue
        l1 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / d
        l2 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / d
        l3 = 1 - l1 - l2
        m = (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
        if not m.any():
            continue
        z = l1 * t[0, 2] + l2 * t[1, 2] + l3 * t[2, 2]
        sub = Z[lo[1]:hi[1] + 1, lo[0]:hi[0] + 1]
        np.maximum(sub, np.where(m, z, -1.0), out=sub)
    return Z


def shade(Z, zmax):
    g = np.where(Z < 0, 0, 70 + 185 * np.clip(Z / max(zmax, 1e-6), 0, 1)).astype(np.uint8)
    img = np.stack([g, g, g], -1)
    img[Z < 0] = (38, 42, 48)
    return Image.fromarray(img)


def outline(mask):
    e = mask & ~(np.roll(mask, 1, 0) & np.roll(mask, -1, 0) & np.roll(mask, 1, 1) & np.roll(mask, -1, 1))
    return e


def compare(name, out=None):
    S, R = stock_tris(name), remake_tris(name)
    allv = np.r_[S.reshape(-1, 3), R.reshape(-1, 3)]
    box = (allv[:, 0].min() - 2, allv[:, 1].min() - 2, allv[:, 0].max() + 2, allv[:, 1].max() + 2)
    zs, zr = raster(S, box), raster(R, box)
    zmax = max(zs.max(), zr.max())
    ms, mr = zs > .5, zr > .5
    iou = (ms & mr).sum() / max(1, (ms | mr).sum())
    a, b = shade(zs, zmax), shade(zr, zmax)
    ea = outline(ms)
    ra = np.array(b)
    ra[ea] = (255, 140, 0)
    b = Image.fromarray(ra)
    W, H = a.size
    m = Image.new('RGB', (2 * W + 30, H + 40), (24, 26, 30))
    m.paste(a, (10, 30))
    m.paste(b, (W + 20, 30))
    d = ImageDraw.Draw(m)
    d.text((10, 8), f'{name} STOCK', fill=(230, 230, 230))
    d.text((W + 20, 8), f'REMAKE (orange = stock outline) IoU {iou:.2f}', fill=(230, 230, 230))
    for img_x in (10, W + 20):           # 5 m grid ticks along the top edge
        for i in range(0, int((box[2] - box[0]) / 5) + 1):
            x = img_x + int(i * 5 * PX)
            d.line([(x, 30), (x, 36)], fill=(120, 120, 120))
    p = Path(out) if out else OUT / name
    p.mkdir(parents=True, exist_ok=True)
    f = p / f'{name}_topdown.png'
    m.save(f)
    return f, iou


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('assets')
    ap.add_argument('--out')
    a = ap.parse_args()
    for n in a.assets.split(','):
        f, iou = compare(n, a.out)
        print(f'[topdown] {n} plan IoU {iou:.2f} -> {f}')


if __name__ == '__main__':
    main()
