"""Grime metrics on UV-used texels: stock texture vs the baked basecolor atlas.

Same measurement on both sides. Value/saturation spread, rust-hue coverage
(hue 5-30 deg, sat >= .5, value <= .55) and dark-stain fraction (value below
.6 x the STOCK texture's median value, same reference for all states). Run: py -3 wear_metrics.py <pass_dir> [names]
"""
import colorsys, json, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

STOCK = Path('G:/mc2-assets/out/scifi-kit/round2/stock')


def hsv(rgb):
    a = np.asarray(rgb, float) / 255.0
    mx, mn = a.max(1), a.min(1)
    d = mx - mn
    s = np.where(mx > 0, d / np.maximum(mx, 1e-6), 0)
    h = np.zeros(len(a))
    nz = d > 1e-6
    r, g, b = a[:, 0], a[:, 1], a[:, 2]
    h = np.where(nz & (mx == r), ((g - b) / np.maximum(d, 1e-6)) % 6, h)
    h = np.where(nz & (mx == g), (b - r) / np.maximum(d, 1e-6) + 2, h)
    h = np.where(nz & (mx == b), (r - g) / np.maximum(d, 1e-6) + 4, h)
    return h * 60.0, s, mx


def metrics(px, ref_med=None):
    h, s, v = hsv(px)
    med = float(np.median(v)) if ref_med is None else ref_med
    rust = (h >= 5) & (h <= 30) & (s >= .5) & (v <= .55)
    return dict(n=int(len(px)), value_mean=round(float(v.mean()), 3), value_std=round(float(v.std()), 3),
                sat_mean=round(float(s.mean()), 3), sat_std=round(float(s.std()), 3),
                rust_cov=round(float(rust.mean()), 4), dark_stain=round(float((v < .6 * med).mean()), 4))


def mask_pixels(img, tris):
    w, h = img.size
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    for t in tris:
        d.polygon([(u * w, v * h) for u, v in t], fill=255)
    mk = np.array(m) > 0
    return np.array(img.convert('RGB'))[mk]


def stock_px(name):
    d = json.loads((STOCK / f'{name}.json').read_text())
    per = {}
    for i, mi in enumerate(d['materials']):
        if d['texture_alpha'][mi] or not d['texture_paths'][mi]:
            continue
        uv = d['uv'][i * 3:i * 3 + 3]
        per.setdefault(mi, []).append([(u, v) for u, v in uv])
    out = []
    for mi, tris in per.items():
        out.append(mask_pixels(Image.open(d['texture_paths'][mi]).convert('RGBA'), [[(u % 1, v % 1) for u, v in t] for t in tris]))
    px = np.concatenate(out)
    return px[~((px[:, 0] > 240) & (px[:, 2] > 240) & (px[:, 1] < 20))]


def mine_px(pass_dir, name, cond):
    import trimesh
    sc = trimesh.load(Path(pass_dir) / 'pbr' / f'{name}_{cond}_lod0.glb', force='scene')
    tris = []
    for g in sc.geometry.values():
        uv = np.asarray(g.visual.uv)
        for f in g.faces:
            tris.append([(uv[i][0], 1 - uv[i][1]) for i in f])
    return mask_pixels(Image.open(Path(pass_dir) / 'pbr' / f'{name}_{cond}_basecolor.png'), tris)


if __name__ == '__main__':
    pd = Path(sys.argv[1])
    names = sys.argv[2].split(',') if len(sys.argv) > 2 else sorted({p.name.split('_pristine')[0] for p in pd.glob('*_pristine_beauty.png')})
    res = {}
    for n in names:
        sp = stock_px(n)
        r = {'stock': metrics(sp)}
        ref = float(np.median(hsv(sp)[2]))
        for c in ('pristine', 'worn', 'abandoned'):
            r[c] = metrics(mine_px(pd, n, c), ref)
        res[n] = r
        print(n, *[f"{k}:{v['value_std']}/{v['sat_std']}/{v['rust_cov']}/{v['dark_stain']}" for k, v in r.items()], flush=True)
    (pd / 'wear-metrics.json').write_text(json.dumps(res, indent=1))

