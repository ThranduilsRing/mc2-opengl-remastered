"""Decode stock TGL shapes that the round-2 stock set lacks (faction/generic walls, gates, posts) into the same JSON
form analyze.load_stock reads (G:/mc2-assets/out/scifi-kit/round2/stock/<name>.json): triangle vertices in authoring
metres (WU / 4; x, z, y of the TGL, bbox-centred in plan, base at z 0), per-vertex UVs, per-triangle material index,
texture names and extracted texture paths, node list, raw bounds in WU.

  py -3 tools/scifi_kit/art/tgl_decode.py k_lightwall,m_heavygate [--check s_mediumwall]

The appearance INI (data/tgl/<name>.ini, loose or in an FST) gives the shape file name. The layout follows
mclib/tgl.h (TG_TypeVertex 28 bytes, TG_TypeTriangle 56 bytes) and the placement survey's TGL reader.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.mc2x_import.fst import fst_entries, decompress_entry  # noqa: E402

INSTALL = Path('A:/Games/mc2-opengl/mc2-win64-v0.4')
OUT = Path('G:/mc2-assets/out/scifi-kit/round2/stock')
_INDEX = None


def _index():
    global _INDEX
    if _INDEX is None:
        _INDEX = {}
        for fst in sorted(INSTALL.glob('*.fst')):
            for e in fst_entries(fst):
                _INDEX.setdefault(e[3].lower().replace('\\', '/'), (fst, e))
    return _INDEX


def read(rel):
    p = INSTALL / rel
    if p.exists():
        return p.read_bytes(), str(p)
    hit = _index().get(rel.lower())
    if hit is None:
        raise FileNotFoundError(rel)
    fst, (off, comp, real, name, is_lz, data) = hit
    return decompress_entry(off, comp, real, is_lz, data), f'{fst}::{name}'


def shape_name(name):
    ini, _ = read(f'data/tgl/{name}.ini')
    txt = ini.decode('latin-1')
    m = re.search(r'FileName0?\s*=\s*"([^"]+)"', txt, re.I)
    return m.group(1) if m else name


def parse(raw):
    _, count, ntex = struct.unpack_from('<III', raw)
    off = 12
    textures = []
    for _ in range(ntex):
        nm = raw[off:off + 268].split(b'\0')[0].decode('latin-1')
        textures.append(nm)
        off += 268
    nodes = []
    for _ in range(count):
        kind, = struct.unpack_from('<I', raw, off)
        off += 4
        verts = np.empty((0, 3), np.float32)
        tris = []
        if kind == 1:
            n, = struct.unpack_from('<I', raw, off)
            off += 4
            verts = np.ndarray((n, 3), '<f4', raw, off, strides=(28, 4)).copy()
            off += n * 28
            m, = struct.unpack_from('<I', raw, off)
            off += 4
            for t in range(m):
                a, b, c, tex, flags = struct.unpack_from('<5I', raw, off)
                uv = struct.unpack_from('<6f', raw, off + 20)
                tris.append((a, b, c, tex, uv))
                off += 56
            k, = struct.unpack_from('<I', raw, off)
            off += 4 + k * 12
        centres = np.asarray(struct.unpack_from('<6f', raw, off), np.float32)
        nm = raw[off + 24:off + 49].split(b'\0')[0].decode('latin-1')
        parent = raw[off + 49:off + 74].split(b'\0')[0].decode('latin-1')
        off += 74
        nodes.append(dict(name=nm, parent=parent, centres=centres, verts=verts, tris=tris))
    return textures, nodes


def decode(name):
    shape = shape_name(name)
    raw, src = read(f'data/tgl/{shape}.tgl')
    textures, nodes = parse(raw)
    names = {n['name'] for n in nodes}
    roots = [n for n in nodes if n['parent'] not in names]
    root = roots[-1]['centres'][:3] if roots else np.zeros(3, np.float32)
    P, UV, MAT, out_nodes = [], [], [], []
    for n in nodes:
        if not n['tris'] or n['name'].lower().startswith('_pab'):
            continue
        v = n['verts'] + n['centres'][3:] + root
        cnt = 0
        for a, b, c, tex, uv in n['tris']:
            if max(a, b, c) >= len(v):
                continue
            P += [v[a], v[b], v[c]]
            UV += [uv[0:2], uv[2:4], uv[4:6]]
            MAT.append(tex)
            cnt += 1
        out_nodes.append(dict(name=n['name'], parent=n['parent'] or 'None', center_wu=n['centres'][3:].tolist(),
                              relative_center_wu=n['centres'][:3].tolist(), helper=False, triangles=cnt))
    P = np.asarray(P, np.float64)
    lo, hi = P.min(0), P.max(0)
    c = (lo + hi) / 2
    V = np.c_[(P[:, 0] - c[0]) / 4, (P[:, 2] - c[2]) / 4, (P[:, 1] - lo[1]) / 4]
    tex_paths = []
    tdir = OUT / 'textures'
    tdir.mkdir(parents=True, exist_ok=True)
    for t in textures:
        base = Path(t.replace('\\', '/')).name
        dst = tdir / base
        if not dst.exists():
            for rel in (f'data/tgl/128/{base}', f'data/tgl/{base}', f'data/textures/{base}', f'data/tgl/256/{base}'):
                try:
                    data, _ = read(rel)
                    dst.write_bytes(data)
                    break
                except FileNotFoundError:
                    continue
        tex_paths.append(str(dst) if dst.exists() else None)
    d = dict(vertices_m=V.tolist(), uv=[list(u) for u in UV], materials=MAT, textures=textures,
             texture_alpha=[t.lower().endswith('x.tga') for t in textures], nodes=out_nodes, source=src,
             bounds=dict(min=lo.tolist(), max=hi.tolist(), size=(hi - lo).tolist(), footprint_wu=[float(hi[0] - lo[0]), float(hi[2] - lo[2])],
                         height_wu=float(hi[1] - lo[1])), texture_paths=tex_paths, decoded_by='tgl_decode.py')
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names')
    ap.add_argument('--check', help='decode an asset that already has a round-2 JSON and compare')
    ap.add_argument('--install', help='install root to read from (default the v0.4 dev install); MC2X-only shapes live in A:/Games/mc2-opengl/MC2X-CVE-G')
    ap.add_argument('--out', help='output dir (default the round-2 stock dir)')
    a = ap.parse_args()
    global INSTALL, OUT
    if a.install:
        INSTALL = Path(a.install)
    if a.out:
        OUT = Path(a.out)
        OUT.mkdir(parents=True, exist_ok=True)
    if a.check:
        old = json.loads((OUT / f'{a.check}.json').read_text())
        new = decode(a.check)
        A_ = np.round(np.array(old['vertices_m']), 3)
        B_ = np.round(np.array(new['vertices_m']), 3)
        A_ -= np.r_[[(A_[:, 0].min() + A_[:, 0].max()) / 2, (A_[:, 1].min() + A_[:, 1].max()) / 2, A_[:, 2].min()]]
        sa = {tuple(x) for x in np.round(A_, 2)}
        sb = {tuple(x) for x in np.round(B_, 2)}
        print('check', a.check, 'old', len(sa), 'new', len(sb), 'common', len(sa & sb))
        return
    for n in a.names.split(','):
        d = decode(n)
        (OUT / f'{n}.json').write_text(json.dumps(d))
        V = np.array(d['vertices_m'])
        print(n, len(V) // 3, 'tris', (V.max(0) - V.min(0)).round(2).tolist(), 'textures', d['textures'], [p is not None for p in d['texture_paths']])


if __name__ == '__main__':
    main()
