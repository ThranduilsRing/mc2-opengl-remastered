"""Hard LOD checks on baked kit exports (owner 2026-10-25).

Per asset (<name>_pristine_lod0/1/2.glb):
  - triangle counts over every mesh fall monotonically (FAIL); LOD1 <= 60 % and LOD2 <= 30 % of LOD0 unless LOD0 is
    tiny (< 2000 tris). The ratio is a WARN, not a FAIL, when the LOD build recorded that the target had to be relaxed
    to keep the silhouette (<name>_lods.json; owner: silhouette beats the triangle target);
  - silhouette: LOD0 and LODn masks from the iso and RTS directions, at a pixel size scaled to LODn's switch distance
    (0.06 m/px at LOD1, 0.12 m/px at LOD2: 4x finer than the 0.24 m/px RTS scale so wire and rails register) differ
    by <= 1.5 % of the LOD0 mask, and no connected piece of the LOD0 mask loses more than half its pixels;
  - every animated stock node (bake sidecar animated_nodes) exists in every LOD with the LOD0 name, parent and
    translation (the engine's animation node cache assumes the LOD0 layout), and its geometry is present and not
    collapsed: each LOD's node-mesh bounding box keeps >= 80 % of the LOD0 extent on every axis;
  - texture mapping is intact: at most 2 % of a LOD's surface area has a texel density (uv area / surface area) more
    than 2x off its material's LOD0 median (cross-chart UVs show as black or streaked atlas at distance).

  py -3 lod_check.py [names...]            # newest bake per asset under intent-pass/_work
  py -3 lod_check.py --glb <..._lod0.glb>   # one export
"""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import meshopt_lods as m  # noqa: E402

OUT = Path('E:/mc2-ref/scifi-kit/art/intent-pass')
CAPS = m.CAPS
TINY = m.TINY
KEEP_EXTENT = .8
UV_OFF_MAX = .02
SIL_PX = (.06, .12)        # mask pixel size (m) at the LOD1 / LOD2 switch distance
SIL_MAX = .015
VIEWS = {'iso': (-1.0, 1.1, 1.0), 'rts': (-1.0, 2.4, 1.25)}   # glb frame (y up), towards the camera


def _world_tris(path):
    doc, b = m.read_glb(path)
    par = {c: i for i, n in enumerate(doc['nodes']) for c in n.get('children', [])}

    def T(i):
        t = np.asarray(doc['nodes'][i].get('translation', [0, 0, 0]), float)
        return (T(par[i]) if i in par else 0) + t
    out = []
    for i, n in enumerate(doc['nodes']):
        if 'mesh' not in n:
            continue
        off = T(i)
        for pr in doc['meshes'][n['mesh']]['primitives']:
            pos = m.accessor(doc, b, pr['attributes']['POSITION']).astype(np.float64) + off
            t = m.accessor(doc, b, pr['indices']).reshape(-1, 3)
            out.append(pos[t])
    return np.concatenate(out) if out else np.zeros((0, 3, 3))


def _mask(V, view, px, frame):
    from PIL import Image, ImageDraw
    d = np.asarray(VIEWS[view], float)
    d /= np.linalg.norm(d)
    r = np.cross([0, 1.0, 0], d)
    r /= np.linalg.norm(r)
    u = np.cross(d, r)
    P = np.stack([V @ r, V @ u], -1)
    lo, size = frame
    im = Image.new('L', size, 0)
    dr = ImageDraw.Draw(im)
    Q = (P - lo) / px
    for t in Q:
        dr.polygon([(x, size[1] - y) for x, y in t], fill=255)
    return np.asarray(im) > 0


def silhouette(lod0, lodn, level):
    """-> list of failure strings for LODn vs LOD0."""
    from scipy import ndimage
    V0, Vn = _world_tris(lod0), _world_tris(lodn)
    px = SIL_PX[level - 1]
    fails = []
    for view in VIEWS:
        d = np.asarray(VIEWS[view], float)
        d /= np.linalg.norm(d)
        r = np.cross([0, 1.0, 0], d)
        r /= np.linalg.norm(r)
        u = np.cross(d, r)
        P = np.stack([V0.reshape(-1, 3) @ r, V0.reshape(-1, 3) @ u], -1)
        lo = P.min(0) - 2 * px
        size = tuple(int(x) for x in np.ceil((P.max(0) - lo) / px + 2))
        if max(size) > 4000:                  # very large assets: coarser grid, same physical threshold
            px2 = px * max(size) / 4000
            size = tuple(int(x) for x in np.ceil((P.max(0) - lo) / px2 + 2))
        else:
            px2 = px
        a, b = _mask(V0, view, px2, (lo, size)), _mask(Vn, view, px2, (lo, size))
        n0 = int(a.sum())
        if not n0:
            continue
        diff = int((a ^ b).sum()) / n0
        if diff > SIL_MAX:
            fails.append(f'LOD{level} {view} silhouette differs by {diff:.1%} of the LOD0 mask (max {SIL_MAX:.1%})')
        lab, k = ndimage.label(a, structure=np.ones((3, 3)))
        if k:
            tot = ndimage.sum(a, lab, range(1, k + 1))
            kept = ndimage.sum(b, lab, range(1, k + 1))
            lost = [i for i in range(k) if tot[i] >= 4 and kept[i] < .5 * tot[i]]
            if lost:
                fails.append(f'LOD{level} {view}: {len(lost)} piece(s) visible in LOD0 are missing (largest {int(max(tot[i] for i in lost))} px)')
    return fails


def _density(path):
    doc, b = m.read_glb(path)
    out = {}
    for mi, mesh in enumerate(doc['meshes']):
        for pr in mesh['primitives']:
            if 'TEXCOORD_0' not in pr['attributes']:
                continue
            pos = m.accessor(doc, b, pr['attributes']['POSITION']).astype(np.float64)
            uv = m.accessor(doc, b, pr['attributes']['TEXCOORD_0']).astype(np.float64)
            t = m.accessor(doc, b, pr['indices']).reshape(-1, 3)
            A = np.linalg.norm(np.cross(pos[t[:, 1]] - pos[t[:, 0]], pos[t[:, 2]] - pos[t[:, 0]]), axis=1) / 2
            e1, e2 = uv[t[:, 1]] - uv[t[:, 0]], uv[t[:, 2]] - uv[t[:, 0]]
            U = np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]) / 2
            k = (mi, pr.get('material'))
            a0, r0 = out.get(k, (np.zeros(0), np.zeros(0)))
            out[k] = (np.r_[a0, A], np.r_[r0, U / np.maximum(A, 1e-9)])
    return out


def uv_off(lod0, lodn):
    """Fraction of LOD surface area whose texel density is > 2x off its material's LOD0 median."""
    base, cur = _density(lod0), _density(lodn)
    bad = tot = 0.0
    for k, (A, r) in cur.items():
        if k not in base:
            continue
        A0, r0 = base[k]
        big = A0 > 1e-4
        if not big.any():
            continue
        med = float(np.median(r0[big]))
        if med <= 0:
            continue
        off = (r > 2 * med) | (r < med / 2)
        bad += float(A[off].sum())
        tot += float(A.sum())
    return bad / max(tot, 1e-9)


def _load(path):
    doc, b = m.read_glb(path)
    tris = sum(doc['accessors'][p['indices']]['count'] // 3 for mm in doc['meshes'] for p in mm['primitives'])
    parents = {c: i for i, n in enumerate(doc['nodes']) for c in n.get('children', [])}
    nodes = {}
    for i, n in enumerate(doc['nodes']):
        ext = None
        if 'mesh' in n:
            P = []
            for p in doc['meshes'][n['mesh']]['primitives']:
                pos = m.accessor(doc, b, p['attributes']['POSITION'])
                idx = m.accessor(doc, b, p['indices']).reshape(-1)
                if len(idx):
                    P.append(pos[idx])
            if P:
                P = np.concatenate(P)
                ext = (P.max(0) - P.min(0)).tolist()
        par = doc['nodes'][parents[i]].get('name') if i in parents else None
        nodes[n.get('name')] = dict(parent=par, t=n.get('translation', [0, 0, 0]), ext=ext)
    return tris, nodes


def check(lod0):
    lod0 = Path(lod0)
    side = Path(str(lod0).replace('_lod0.glb', '_lod0.asset.json'))
    anim = [n['name'] for n in json.loads(side.read_text()).get('animated_nodes', [])] if side.exists() else []
    paths = [lod0] + [Path(str(lod0).replace('_lod0.glb', f'_lod{i}.glb')) for i in (1, 2)]
    fails = []
    if not all(p.exists() for p in paths):
        return dict(fails=['missing LOD files'], tris=[], warns=[])
    loaded = [_load(p) for p in paths]
    tris = [t for t, _ in loaded]
    t0 = tris[0]
    warns = []
    rec = Path(str(lod0).replace('_lod0.glb', '_lods.json'))
    relaxed = {}
    if rec.exists():
        relaxed = {i + 1: x.get('relaxed', False) for i, x in enumerate(json.loads(rec.read_text())['lods'])}
    for i in (1, 2):
        if tris[i] > tris[i - 1]:
            fails.append(f'LOD{i} has more triangles than LOD{i - 1} ({tris[i]} > {tris[i - 1]})')
        if t0 >= TINY and tris[i] > CAPS[i - 1] * t0 + 1:
            msg = f'LOD{i} {tris[i]} tris is {tris[i] / t0:.0%} of LOD0 (target {CAPS[i - 1]:.0%})'
            (warns if relaxed.get(i) else fails).append(msg + (' - relaxed: silhouette first' if relaxed.get(i) else ''))
        fails += silhouette(paths[0], paths[i], i)
    for i in (1, 2):
        f = uv_off(paths[0], paths[i])
        if f > UV_OFF_MAX:
            fails.append(f'LOD{i} texture mapping: {f:.1%} of the area is off the LOD0 texel density (max {UV_OFF_MAX:.0%})')
    n0 = loaded[0][1]
    for name in anim:
        if name not in n0:
            fails.append(f'LOD0 lacks animated node {name}')
            continue
        for i in (1, 2):
            ni = loaded[i][1].get(name)
            if ni is None:
                fails.append(f'LOD{i} lacks animated node {name}')
                continue
            if ni['parent'] != n0[name]['parent']:
                fails.append(f'LOD{i} {name} parent {ni["parent"]} != LOD0 {n0[name]["parent"]}')
            if np.abs(np.subtract(ni['t'], n0[name]['t'])).max() > 1e-4:
                fails.append(f'LOD{i} {name} pivot moved')
            e0, ei = n0[name]['ext'], ni['ext']
            if e0 is None:
                continue
            if ei is None:
                fails.append(f'LOD{i} {name} has no geometry')
            elif any(b_ < KEEP_EXTENT * a_ - 1e-3 for a_, b_ in zip(e0, ei)):
                fails.append(f'LOD{i} {name} geometry collapsed (extent {np.round(ei, 2).tolist()} vs LOD0 {np.round(e0, 2).tolist()})')
    return dict(fails=fails, tris=tris, warns=warns)


def newest():
    best = {}
    for p in OUT.glob('_work/*/pbr/*_pristine_lod0.glb'):
        n = p.name.removesuffix('_pristine_lod0.glb')
        if not (p.parent / f'{n}_pristine-bake.json').exists():     # stray glbs (no bake record) are not bakes
            continue
        if n not in best or p.stat().st_mtime > best[n].stat().st_mtime:
            best[n] = p
    return best


def main():
    args = sys.argv[1:]
    if args[:1] == ['--glb']:
        r = check(args[1])
        print(r)
        raise SystemExit(1 if r['fails'] else 0)
    best = newest()
    names = args or sorted(best)
    bad = []
    for n in names:
        r = check(best[n])
        if r['fails']:
            bad.append(n)
            print(f'FAIL {n} {r["tris"]} ({best[n].parent.parent.name}): ' + '; '.join(r['fails']))
        elif r.get('warns'):
            print(f'WARN {n} {r["tris"]}: ' + '; '.join(r['warns']))
    print(f'{len(names)} checked, {len(bad)} fail: {",".join(bad)}')
    raise SystemExit(1 if bad else 0)


if __name__ == '__main__':
    main()
