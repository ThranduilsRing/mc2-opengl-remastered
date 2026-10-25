"""Read the stock massing (offline, no Blender): decompose a stock mesh's height field into axis-aligned blocks.

    py -3 tools/scifi_kit/art/stock_massing.py s_hq,l_hq            -> assets/military_massing.json (merged)

Each block is (x0, y0, x1, y1, h) in kit metres (the frame the validation uses), largest first. Builders read
the JSON (Blender never loads stock data), so the remake keeps the stock silhouette: towers stay towers,
annexes stay where the stock has them.
"""
import json
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
OUT = HERE / 'assets' / 'military_massing.json'


def height_field(name, cell=.6):
    import analyze
    from intent import _sample_tris
    V = analyze.load_stock(name)['V']
    P = V.reshape(-1, 3)
    lo, hi = P.min(0), P.max(0)
    nx, ny = max(4, math.ceil((hi[0] - lo[0]) / cell)), max(4, math.ceil((hi[1] - lo[1]) / cell))
    N = max(nx, ny)
    box = (lo[0], lo[0] + N * cell, lo[1], lo[1] + N * cell)
    g = np.zeros((N, N))
    _sample_tris(V, g, box, N)
    g = g[:ny, :nx]
    # close sampling pinholes: a zero cell with high neighbours takes their median
    z = np.pad(g, 1)
    for j in range(ny):
        for i in range(nx):
            if g[j, i] < .2:
                nb = z[j:j + 3, i:i + 3].ravel()
                if (nb > .2).sum() >= 6:
                    g[j, i] = np.median(nb[nb > .2])
    return g, lo[0], lo[1], cell


def largest_rect(mask):
    """Largest all-true axis-aligned rectangle (histogram method): (area, j0, j1, i0, i1) inclusive-exclusive."""
    ny, nx = mask.shape
    hgt = np.zeros(nx, int)
    best = (0, 0, 0, 0, 0)
    for j in range(ny):
        hgt = np.where(mask[j], hgt + 1, 0)
        stack = []
        for i in range(nx + 1):
            h = hgt[i] if i < nx else 0
            start = i
            while stack and stack[-1][1] >= h:
                s, sh = stack.pop()
                area = sh * (i - s)
                if area > best[0]:
                    best = (area, j - sh + 1, j + 1, s, i)
                start = s
            stack.append((start, h))
    return best


def blocks(name, cell=.6, step=.5, min_area=4.0, min_h=.6):
    g, x0, y0, c = height_field(name, cell)
    q = np.round(g / step) * step
    done = q < min_h
    out, slivers = [], []
    while True:
        best = None
        for h in sorted(set(q[~done].ravel()), reverse=True):
            m = (~done) & (np.abs(q - h) <= step * 1.01)
            a = largest_rect(m)
            if a[0] * c * c >= min_area and (best is None or a[0] > best[0][0]):
                best = (a, h)
        if best is None:
            break
        (area, j0, j1, i0, i1), h = best
        short, long_ = min(j1 - j0, i1 - i0), max(j1 - j0, i1 - i0)
        if short < 2 or (short == 2 and long_ < 4):      # slivers (thin panels, masts, edges) are not masses
            done[j0:j1, i0:i1] = True
            slivers.append((x0 + (i0 + i1) / 2 * c, y0 + (j0 + j1) / 2 * c, float(g[j0:j1, i0:i1].max()), short * c))
            continue
        hh = float(np.percentile(g[j0:j1, i0:i1], 90))
        out.append([round(float(x0 + i0 * c), 2), round(float(y0 + j0 * c), 2), round(float(x0 + i1 * c), 2), round(float(y0 + j1 * c), 2),
                    round(hh, 2)])
        done[j0:j1, i0:i1] = True
    top = max((b[4] for b in out), default=0.0)
    masts = [[round(float(a), 2), round(float(b_), 2), round(h, 2), round(w, 2)] for a, b_, h, w in slivers if h > top + 1.5]
    return dict(cell=c, origin=[round(float(x0), 3), round(float(y0), 3)], grid=np.round(g, 2).tolist(), blocks=merge(out), masts=masts)


def merge(blocks, dh=1.6):
    """Join side-by-side blocks that share a full edge into one block with a roof profile along the join axis
    (stock arched, sloped and stepped roofs come out of the grid as strips; they are one mass)."""
    B = [dict(box=b[:4], h=b[4], axis=None, prof=[((b[0] + b[2]) / 2, b[4]), ((b[1] + b[3]) / 2, b[4])]) for b in blocks]
    for b in B:
        b['prof'] = None
    eps = 1e-3
    changed = True
    while changed:
        changed = False
        for i, a in enumerate(B):
            for j, b in enumerate(B):
                if i == j:
                    continue
                ax0, ay0, ax1, ay1 = a['box']
                bx0, by0, bx1, by1 = b['box']
                for axis, ok in (('x', abs(ay0 - by0) < eps and abs(ay1 - by1) < eps and abs(ax1 - bx0) < eps),
                                 ('y', abs(ax0 - bx0) < eps and abs(ax1 - bx1) < eps and abs(ay1 - by0) < eps)):
                    if not ok or a['axis'] not in (None, axis) or b['axis'] not in (None, axis):
                        continue
                    pa = a['prof'] or [((ax0 + ax1) / 2 if axis == 'x' else (ay0 + ay1) / 2, a['h'])]
                    pb = b['prof'] or [((bx0 + bx1) / 2 if axis == 'x' else (by0 + by1) / 2, b['h'])]
                    wa = (ax1 - ax0) if axis == 'x' else (ay1 - ay0)
                    wb = (bx1 - bx0) if axis == 'x' else (by1 - by0)
                    if abs(pa[-1][1] - pb[0][1]) > dh or min(a['h'], b['h']) < 1.5 or min(wa if not a['prof'] else 0, wb) > 3.0:
                        continue
                    a['box'] = [ax0, ay0, bx1, by1]
                    a['axis'] = axis
                    a['prof'] = [list(p) for p in pa + pb]
                    a['h'] = max(a['h'], b['h'])
                    B.pop(j)
                    changed = True
                    break
                if changed:
                    break
            if changed:
                break
    return B


def main():
    names = sys.argv[1].split(',')
    data = json.loads(OUT.read_text()) if OUT.exists() else {}
    for n in names:
        data[n] = blocks(n)
        print(n, len(data[n]['blocks']), 'blocks:', data[n]['blocks'][:8])
    OUT.write_text(json.dumps(data, separators=(',', ':')))


if __name__ == '__main__':
    main()
