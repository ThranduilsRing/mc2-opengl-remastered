"""LOD1/LOD2 from the baked LOD0 glb (owner 2026-10-24: raise budgets, bake down; 2026-10-25: thin-chart LODs).

Each primitive's index buffer is simplified on its own vertex buffer (UV/material seams
stay put: meshoptimizer only collapses along matching attributes); everything else
in the glb (materials, textures, light sockets, extras) is kept as is.
  py -3 meshopt_lods.py <..._lod0.glb> [--ratios .45,.18]
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

COMP = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def read_glb(path):
    b = Path(path).read_bytes()
    _, _, total = struct.unpack_from('<III', b, 0)
    jl, _ = struct.unpack_from('<II', b, 12)
    doc = json.loads(b[20:20 + jl])
    off = 20 + jl
    bl, _ = struct.unpack_from('<II', b, off)
    return doc, bytearray(b[off + 8:off + 8 + bl])


def write_glb(path, doc, binary):
    while len(binary) % 4:
        binary += b'\0'
    doc['buffers'][0]['byteLength'] = len(binary)
    js = json.dumps(doc, separators=(',', ':')).encode()
    js += b' ' * ((4 - len(js) % 4) % 4)
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(binary), 0x004E4942) + bytes(binary)
    Path(path).write_bytes(out)


def accessor(doc, binary, i):
    a = doc['accessors'][i]
    bv = doc['bufferViews'][a['bufferView']]
    dt = COMP[a['componentType']]
    nc = NCOMP[a['type']]
    start = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = bv.get('byteStride') or nc * np.dtype(dt).itemsize
    raw = np.frombuffer(bytes(binary[start:start + stride * a['count']]), dtype=np.uint8).reshape(a['count'], stride)
    return raw[:, :nc * np.dtype(dt).itemsize].copy().view(dt).reshape(a['count'], nc)


def add_indices(doc, binary, idx):
    while len(binary) % 4:
        binary += b'\0'
    start = len(binary)
    data = idx.astype(np.uint32).tobytes()
    binary += data
    doc['bufferViews'].append(dict(buffer=0, byteOffset=start, byteLength=len(data), target=34963))
    doc['accessors'].append(dict(bufferView=len(doc['bufferViews']) - 1, componentType=5125, count=int(len(idx)), type='SCALAR'))
    return len(doc['accessors']) - 1


def components(idx, n):
    """Connected components of a triangle index buffer (= UV charts / separate parts of the export)."""
    parent = np.arange(n)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for a_, b_, c_ in idx.reshape(-1, 3):
        ra = find(a_)
        parent[find(b_)] = ra
        parent[find(c_)] = ra
    return np.array([find(v) for v in range(n)])


# LOD rules (owner 2026-10-25): reduce detail, never parts. Triangle counts fall monotonically and aim at LOD1 <= 60 %
# and LOD2 <= 30 % of LOD0 (tiny assets reuse LOD0); silhouette beats the target: when an asset cannot get there
# without dropping visible parts it keeps the higher count, and the achieved ratio is recorded (<name>_lods.json).
#
# How: the baked export splits every vertex at each UV seam and hard edge, so edge-collapse simplification either does
# nothing (every edge is a border) or, on welded topology, drags triangles across UV charts (black/streaked atlas).
# Instead LODs drop UV charts, thinnest first, under two guards: a chart goes only if its physical part keeps a body
# chart wider than the level's cap (bevel strips on a box go, the box stays; rails, wire, pipes and antennas, whose
# charts are all thin, are never touched); a whole part goes only at LOD2 and only when it is a few centimetres
# across (rivets, bolt heads). Every kept triangle keeps its exact UVs and normals. Animated nodes keep every part.
CAPS = (.6, .3)
TINY = 2000
MAX_WIDTH = (.05, .10)    # widest chart a level may drop, glb metres ("a few cm": subpixel at the switch distance)
SUBPIXEL_PART = .12       # LOD2 only: whole parts this small (bbox diagonal, m) may go
BODY = .10                # a part with a chart wider than this has a body; thinner parts (wire, rails) stay whole


def _parts(pos, idx):
    """Physical part id per vertex: charts joined where they share positions."""
    _, inv = np.unique(np.round(pos.astype(np.float64), 4), axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    return components(inv[idx].astype(np.int64), int(inv.max()) + 1)[inv]


def chart_widths(pos, idx):
    """-> (chart id per triangle, {chart: width}); width = extent along the chart's second principal axis."""
    ch = components(idx, len(pos))
    t = idx.reshape(-1, 3)
    tc = ch[t[:, 0]]
    w = {}
    for c in np.unique(tc):
        vs = np.unique(t[tc == c])
        X = pos[vs].astype(np.float64)
        if len(vs) < 3:
            w[int(c)] = 0.0
            continue
        _, _, vt = np.linalg.svd(X - X.mean(0), full_matrices=False)
        e = (X - X.mean(0)) @ vt[1]
        w[int(c)] = float(e.max() - e.min())
    return tc, w


def animated_meshes(doc, lod0):
    """Mesh indices of the animated stock nodes (from the bake sidecar)."""
    side = Path(str(lod0).replace('_lod0.glb', '_lod0.asset.json'))
    names = set()
    if side.exists():
        names = {n['name'] for n in json.loads(side.read_text()).get('animated_nodes', [])}
    return {n['mesh'] for n in doc['nodes'] if 'mesh' in n and n.get('name') in names}


def lods(lod0, ratios=(.5, .25), protect=()):
    """-> (LOD0 triangles over every mesh, [LOD1, LOD2 records]); also writes <name>_lods.json with the achieved
    ratios and whether the target had to be relaxed. protect: kept for the call signature (opening covers are wide
    charts and never go)."""
    doc, binary = read_glb(lod0)
    base_tris = sum(doc['accessors'][p['indices']]['count'] // 3 for m in doc['meshes'] for p in m['primitives'])
    anim = animated_meshes(doc, lod0)
    tiny = base_tris < TINY
    prims, charts = [], []
    for mi, m in enumerate(doc['meshes']):
        for pi, prim in enumerate(m['primitives']):
            pos = accessor(doc, binary, prim['attributes']['POSITION'])
            idx = accessor(doc, binary, prim['indices']).reshape(-1).astype(np.uint32)
            tc, w = chart_widths(pos, idx)
            tri = idx.reshape(-1, 3)
            phys = _parts(pos, idx)[tri[:, 0]]
            part_of, pdiag, body = {}, {}, {}
            for c in w:
                sel = tc == c
                part_of[c] = int(phys[sel][0])
            for p_ in np.unique(phys):
                q = pos[tri[phys == p_].reshape(-1)]
                pdiag[int(p_)] = float(np.linalg.norm(q.max(0) - q.min(0)))
            for c, wc in w.items():                          # widest chart per part = its body
                body[part_of[c]] = max(body.get(part_of[c], 0.0), wc)
            k = len(prims)
            prims.append((mi, pos, idx, tc, phys))
            for c, wc in w.items():
                charts.append(dict(w=wc, k=k, c=c, n=int((tc == c).sum()), part=(k, part_of[c]),
                                   body=body[part_of[c]], pdiag=pdiag[part_of[c]], anim=mi in anim))
    charts.sort(key=lambda x: x['w'])
    out = []
    prev = set()                           # LOD2 starts from LOD1's drop set: it can only remove more
    for li, ratio in enumerate(ratios, 1):
        path = Path(str(lod0).replace('_lod0.glb', f'_lod{li}.glb'))
        cap_w = MAX_WIDTH[li - 1]
        while True:
            d = json.loads(json.dumps(doc))
            b = bytearray(binary)
            drop = set(prev)
            total = base_tris - sum(ch['n'] for ch in charts if (ch['k'], ch['c']) in drop)
            if not tiny:
                for ch in charts:
                    if total <= ratio * base_tris or ch['w'] > cap_w:
                        break
                    if (ch['k'], ch['c']) in drop:
                        continue
                    detail = ch['body'] > BODY                  # the part keeps a wider body chart
                    rivet = li == 2 and not ch['anim'] and ch['pdiag'] <= SUBPIXEL_PART
                    if detail or rivet:
                        drop.add((ch['k'], ch['c']))
                        total -= ch['n']
            news = []
            for k, (mi, pos, idx, tc, phys) in enumerate(prims):
                keep = np.array([(k, int(c)) not in drop for c in tc], bool)
                news.append(idx.reshape(-1, 3)[keep].reshape(-1) if keep.any() else idx[:3])
            tris = sum(len(n) // 3 for n in news)
            prim_list = [p for m in d['meshes'] for p in m['primitives']]
            for prim, new in zip(prim_list, news):
                prim['indices'] = add_indices(d, b, new)
            write_glb(path, d, b)
            if drop == prev or tiny:
                break
            import lod_check                       # silhouette guard: a level that changes the outline backs off
            if not lod_check.silhouette(lod0, path, li):
                break
            if cap_w < .012:
                drop = set(prev)                   # nothing safe to drop at this level: keep the previous level
                news = []
                for k, (mi, pos, idx, tc, phys) in enumerate(prims):
                    keep = np.array([(k, int(c)) not in drop for c in tc], bool)
                    news.append(idx.reshape(-1, 3)[keep].reshape(-1) if keep.any() else idx[:3])
                tris = sum(len(n) // 3 for n in news)
                d = json.loads(json.dumps(doc))
                b = bytearray(binary)
                for prim, new in zip([p for m in d['meshes'] for p in m['primitives']], news):
                    prim['indices'] = add_indices(d, b, new)
                write_glb(path, d, b)
                break
            cap_w /= 2
        prev = drop
        r = tris / max(1, base_tris)
        out.append(dict(path=path.as_posix(), triangles=tris, ratio=round(r, 3), target=CAPS[li - 1], max_chart_width=round(cap_w, 4),
                        relaxed=bool(not tiny and r > CAPS[li - 1]), distance_wu=(0, 350, 700)[li],
                        simplifier='thin-chart detail drop' + (' (LOD0 reused: tiny)' if tiny else '')))
        print('[lods]', path.name, tris, 'tris', f'{r:.0%} of LOD0' + (' (target relaxed: silhouette first)' if out[-1]['relaxed'] else ''), flush=True)
    Path(str(lod0).replace('_lod0.glb', '_lods.json')).write_text(json.dumps(dict(lod0_triangles=base_tris, lods=out), indent=2))
    return base_tris, out


def main():
    ratios = (.5, .25)
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--ratios' in sys.argv:
        ratios = tuple(float(x) for x in sys.argv[sys.argv.index('--ratios') + 1].split(','))
        args = [a for a in args if a != sys.argv[sys.argv.index('--ratios') + 1]]
    for p in args:
        lods(p, ratios)


if __name__ == '__main__':
    main()


def opening_covers(k):
    """World boxes of opening covers on a built kit, in the glb frame (x, z, -y)."""
    out = []
    for pc in k.pieces:
        if pc.get('door') or pc.get('pane') or pc.get('feature') or pc['m'] in ('paint.door', 'glass'):
            v = np.asarray(pc['v'], float)
            g = np.stack([v[:, 0], v[:, 2], -v[:, 1]], axis=1)
            out.append((g.min(0), g.max(0)))
    return out
