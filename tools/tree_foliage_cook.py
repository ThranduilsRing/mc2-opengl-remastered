"""Offline tree foliage cook: bark vs leaf split, chained capped bark simplification,
aggregate (nested-subset, area-preserving) leaf-card LOD, far proxy, coverage gates.

  py -3 tools/tree_foliage_cook.py <source_lod0.glb> <texture_dir> <out_dir> [--sheet]

LOD0 is the source file copied unchanged. Writes lod1..lod3 and far next to it, a
<stem>.report.json and (with --sheet) a contact sheet. Exits 1 if any gate misses.
Textures are external TGA files named by the glb image uris (texture_dir).
"""
import copy
import ctypes
import json
import shutil
import sys
from pathlib import Path

import numpy as np
import scipy.sparse as sp
from PIL import Image
from scipy.sparse.csgraph import connected_components

sys.path.insert(0, str(Path(__file__).parent / 'scifi_kit/art'))
from meshopt_lods import read_glb, write_glb, accessor  # noqa: E402

LEVELS = ('lod0', 'lod1', 'lod2', 'lod3', 'far')
# per-level bark: tri ratio vs source mesh, absolute error cap as a fraction of the tree bbox diagonal
BARK_RATIO = (1.0, .15, .05, .012)
BARK_ERR = (0., .004, .012, .03, .10)
BARK_FAR_TRIS = 300
LEAF_FRAC = (1.0, 1.0, .35)       # lod1 = hulls of every card, lod2 = 35% of cards
LEAF_LOD3_SHARE = .008            # lod3 leaf tris as a share of LOD0 total tris
LEAF_FAR_TRIS = 1200
GATE_LOD3 = .015
GATE_FAR = 2000
GATE_BYTES = 1.3
# low-pass (8x8 px blocks of a 256 px view) alpha density error vs LOD0, mean over 9 views, per level
GATE_COVERAGE = (0., .10, .20, .30, .45)
FLOAT_ATTRS = ('POSITION', 'NORMAL', 'TEXCOORD_0', 'TEXCOORD_1', 'COLOR_0')


# --------------------------------------------------------------------------- model
def load(path, texdir):
    """-> dict(doc, meshes=[dict(node, cls, attrs, idx, tex)], tex={index: rgba array})"""
    doc, bin_ = read_glb(path)
    for n in doc['nodes']:
        assert not ({'matrix', 'translation', 'rotation', 'scale'} & set(n)), 'node transforms not supported'
    meshes, texcache = [], {}
    for ni, node in enumerate(doc['nodes']):
        if 'mesh' not in node:
            continue
        prim = doc['meshes'][node['mesh']]['primitives']
        assert len(prim) == 1 and prim[0].get('mode', 4) == 4
        p = prim[0]
        mat = doc['materials'][p['material']]
        attrs = {}
        for k, a in p['attributes'].items():
            arr = accessor(doc, bin_, a)
            if arr.dtype == np.uint8:
                arr = arr.astype(np.float32) / 255
            attrs[k] = arr.astype(np.float32)
        tex = mat['pbrMetallicRoughness']['baseColorTexture']['index']
        if tex not in texcache:
            uri = doc['images'][doc['textures'][tex]['source']]['uri']
            texcache[tex] = np.asarray(Image.open(Path(texdir) / uri).convert('RGBA'))
        meshes.append(dict(node=ni, cls='leaf' if mat.get('alphaMode') in ('MASK', 'BLEND') else 'bark',
                           attrs=attrs, idx=accessor(doc, bin_, p['indices']).ravel().astype(np.uint32), tex=tex))
    return dict(doc=doc, meshes=meshes, tex=texcache)


def geometry_bytes(meshes):
    return sum(len(m['attrs']['POSITION']) * sum(a.shape[1] for a in m['attrs'].values()) * 4 + len(m['idx']) * 4 for m in meshes)


def tris(meshes, cls=None):
    return sum(len(m['idx']) // 3 for m in meshes if cls in (None, m['cls']))


def components(idx, n):
    t = idx.reshape(-1, 3)
    a = sp.coo_matrix((np.ones(len(t) * 2), (np.r_[t[:, 0], t[:, 1]], np.r_[t[:, 1], t[:, 2]])), shape=(n, n))
    return connected_components(a, directed=False)[1]


def border_fraction(idx, n):
    t = idx.reshape(-1, 3)
    e = np.sort(np.r_[t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]], axis=1)
    key = e[:, 0].astype(np.int64) * n + e[:, 1]
    u, c = np.unique(key, return_counts=True)
    bv = np.unique(np.r_[u[c == 1] // n, u[c == 1] % n])
    used = len(np.unique(idx))
    return len(bv) / max(used, 1)


def component_report(model):
    out = []
    for m in model['meshes']:
        n = len(m['attrs']['POSITION'])
        lab = components(m['idx'], n)
        cnt = np.bincount(lab[m['idx'].reshape(-1, 3)[:, 0]])
        cnt = cnt[cnt > 0]
        out.append(dict(node=m['node'], cls=m['cls'], tris=len(m['idx']) // 3, cards=int(len(cnt)),
                        tris_per_card=dict(min=int(cnt.min()), median=float(np.median(cnt)), max=int(cnt.max())),
                        border_vertex_fraction=round(border_fraction(m['idx'], n), 4)))
    return out


# --------------------------------------------------------------------------- bark
def _lib():
    from meshoptimizer import _loader
    lib = _loader.lib
    p, n = ctypes.c_void_p, ctypes.c_size_t
    f = lib.meshopt_simplifyWithAttributes
    f.argtypes = [p, p, n, p, n, n, p, n, p, n, p, n, ctypes.c_float, ctypes.c_uint, p]
    f.restype = n
    return lib


def simplify_bark(attrs, idx, target_tris, abs_err, with_attrs=True, sloppy_ok=False):
    """No border lock, Prune on, absolute error cap. Returns index array (never empty).
    with_attrs=False (lod3/far) collapses on position only: UV/normal seams otherwise stall disjoint twigs."""
    lib = _lib()
    pos = np.ascontiguousarray(attrs['POSITION'])
    sec = np.ascontiguousarray(np.concatenate([attrs['NORMAL'], attrs.get('TEXCOORD_0', np.zeros((len(pos), 0), np.float32))], axis=1))
    w = np.array([.5] * 3 + [1.] * (sec.shape[1] - 3), np.float32)
    if not with_attrs:
        sec, w = np.zeros((len(pos), 1), np.float32), np.zeros(1, np.float32)
    out, err = np.empty_like(idx), ctypes.c_float(0)
    c = lib.meshopt_simplifyWithAttributes(out.ctypes.data, idx.ctypes.data, len(idx), pos.ctypes.data, len(pos), 12,
                                           sec.ctypes.data, sec.shape[1] * 4, w.ctypes.data, len(w), None,
                                           max(3, target_tris) * 3, abs_err, 8 | 4, ctypes.byref(err))
    if c < 6:  # pruned away entirely: keep the biggest surviving shape of the previous level instead
        c = lib.meshopt_simplifyWithAttributes(out.ctypes.data, idx.ctypes.data, len(idx), pos.ctypes.data, len(pos), 12,
                                               sec.ctypes.data, sec.shape[1] * 4, w.ctypes.data, len(w), None,
                                               6, abs_err, 4, ctypes.byref(err))
    if c < 6:
        return idx[:6].copy(), False
    if sloppy_ok and c // 3 > 1.5 * target_tris:
        # topology-preserving collapse stalled (non-manifold/duplicated shells): vertex-cluster instead
        import meshoptimizer as mo
        ext = float(np.ptp(pos, axis=0).max())
        out2 = np.empty_like(idx)
        n2 = mo.simplify_sloppy(out2, idx, pos, target_index_count=max(3, target_tris) * 3, target_error=min(1., abs_err / ext))
        if n2 >= 6:
            return out2[:n2].copy(), True
    return out[:c].copy(), False


def dedupe_tris(pos, idx):
    """Drop coincident triangles (same welded vertex triple, either winding). Some source bark
    carries every face twice (4 faces per edge), which meshopt classes non-manifold and locks."""
    _, inv = np.unique(np.round(pos, 4), axis=0, return_inverse=True)
    t = np.sort(inv.ravel()[idx.reshape(-1, 3)], axis=1)
    _, first = np.unique(t, axis=0, return_index=True)
    return idx.reshape(-1, 3)[np.sort(first)].ravel().copy()


def compact(attrs, idx):
    used, inv = np.unique(idx, return_inverse=True)
    return {k: v[used] for k, v in attrs.items()}, inv.astype(np.uint32)


# --------------------------------------------------------------------------- leaf cards
def card_hulls(m, tex):
    """Collapse each leaf component to its oriented hull rectangle (2 tris) with fitted UVs.
    Returns per-card arrays and visible alpha area (priority weight)."""
    a = m['attrs']
    pos = a['POSITION'].astype(np.float64)
    idx = m['idx'].reshape(-1, 3)
    lab = components(m['idx'], len(pos))
    used = np.zeros(len(pos), bool)
    used[m['idx']] = True
    ids = np.unique(lab[used])
    remap = np.zeros(lab.max() + 1, np.int64)
    remap[ids] = np.arange(len(ids))
    nc = len(ids)
    vl = remap[lab[used]]
    P = pos[used]

    def msum(v):
        return np.stack([np.bincount(vl, v[:, j], nc) for j in range(v.shape[1])], 1)
    cnt = np.bincount(vl, minlength=nc).astype(np.float64)
    mean = msum(P) / cnt[:, None]
    D = P - mean[vl]
    cov = np.stack([np.stack([np.bincount(vl, D[:, i] * D[:, j], nc) for j in range(3)], 1) for i in range(3)], 1) / cnt[:, None, None]
    w_, v_ = np.linalg.eigh(cov)
    e1, e2, en = v_[:, :, 2], v_[:, :, 1], v_[:, :, 0]
    nrm = msum(a['NORMAL'][used].astype(np.float64))
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
    en = en * np.where((en * nrm).sum(1) < 0, -1, 1)[:, None]
    # PCA axes have arbitrary handedness; front faces must agree with the authored normal.
    e2 = e2 * np.where((np.cross(e1, e2) * en).sum(1) < 0, -1, 1)[:, None]
    A, B = (D * e1[vl]).sum(1), (D * e2[vl]).sum(1)
    order = np.argsort(vl, kind='stable')
    starts = np.r_[0, np.cumsum(cnt.astype(np.int64))[:-1]]
    amin, amax = (f.reduceat(A[order], starts) for f in (np.minimum, np.maximum))
    bmin, bmax = (f.reduceat(B[order], starts) for f in (np.minimum, np.maximum))
    ca, cb = np.stack([amin, amax, amax, amin], 1), np.stack([bmin, bmin, bmax, bmax], 1)
    corners = mean[:, None, :] + ca[:, :, None] * e1[:, None, :] + cb[:, :, None] * e2[:, None, :]
    # affine UV fit per card: uv = c + a*Ua + b*Ub (normal equations)
    X = np.stack([np.ones_like(A), A, B], 1)
    XtX = np.stack([np.stack([np.bincount(vl, X[:, i] * X[:, j], nc) for j in range(3)], 1) for i in range(3)], 1) + np.eye(3) * 1e-12
    cards = dict(corners=corners, normal=en, nc=nc)
    uvres = 0.
    for key in ('TEXCOORD_0', 'TEXCOORD_1'):
        if key not in a:
            continue
        uv = a[key][used].astype(np.float64)
        coef = np.linalg.solve(XtX, np.stack([msum(X * uv[:, [j]]) for j in range(2)], 2))  # (nc,3,2)
        fit = np.einsum('vi,vij->vj', X, coef[vl])
        uvres = max(uvres, float(np.abs(fit - uv).max())) if key == 'TEXCOORD_0' else uvres
        Xc = np.stack([np.ones_like(ca), ca, cb], 2)
        cards[key] = np.einsum('nki,nij->nkj', Xc, coef)
    if 'COLOR_0' in a:
        cards['COLOR_0'] = msum(a['COLOR_0'][used].astype(np.float64)) / cnt[:, None]
    cards['uv_fit_max_residual'] = uvres
    # visible alpha area: hull area x opaque fraction of its UV window (8x8 nearest samples)
    uvc = cards['TEXCOORD_0']
    lo, hi = uvc.min(1), uvc.max(1)
    g = (np.arange(8) + .5) / 8
    s = lo[:, None, None, :] + (hi - lo)[:, None, None, :] * np.stack(np.meshgrid(g, g), -1)[None]
    frac = alpha_at(tex, s.reshape(nc, 64, 2)).mean(1)
    area = (amax - amin) * (bmax - bmin)
    cards['weight'] = np.maximum(area * frac, 1e-9)
    cards['centroid'] = corners.mean(1)
    return cards


def alpha_at(tex, uv, cutoff=.5):
    h, w = tex.shape[:2]
    x = np.floor((uv[..., 0] % 1.) * w).astype(int).clip(0, w - 1)
    y = np.floor((uv[..., 1] % 1.) * h).astype(int).clip(0, h - 1)
    return tex[y, x, 3] >= cutoff * 255


def leaf_level(m, cards, order, frac_cards=None, n_keep=None):
    """Nested subset: first n_keep cards of `order`; survivors scaled about their centroid by
    sqrt(total visible area / kept visible area). Returns attrs, idx."""
    k = max(1, min(cards['nc'], n_keep if n_keep is not None else int(round(cards['nc'] * frac_cards))))
    sel = order[:k]
    s = np.sqrt(cards['weight'].sum() / cards['weight'][sel].sum()) if k < cards['nc'] else 1.
    c = cards['centroid'][sel][:, None, :]
    pos = (c + s * (cards['corners'][sel] - c)).reshape(-1, 3)
    attrs = {'POSITION': pos.astype(np.float32), 'NORMAL': np.repeat(cards['normal'][sel], 4, axis=0).astype(np.float32)}
    for key in ('TEXCOORD_0', 'TEXCOORD_1'):
        if key in cards:
            attrs[key] = cards[key][sel].reshape(-1, 2).astype(np.float32)
    if 'COLOR_0' in cards:
        attrs['COLOR_0'] = np.repeat(cards['COLOR_0'][sel], 4, axis=0).astype(np.float32)
    attrs = {k_: attrs[k_] for k_ in m['attrs']}
    base = np.arange(k, dtype=np.uint32)[:, None] * 4
    idx = (base + np.array([0, 1, 2, 0, 2, 3], np.uint32)).ravel()
    return attrs, idx, float(s)


# --------------------------------------------------------------------------- raster (ortho)
def view_basis(az, el):
    az, el = np.radians(az), np.radians(el)
    d = np.array([np.cos(el) * np.sin(az), -np.sin(el), np.cos(el) * np.cos(az)])   # towards camera; source trees grow toward -Y
    up = np.array([0, -1, 0.]) if abs(np.sin(el)) < .99 else np.array([0, 0, -1.])
    r = np.cross(up, d)
    r /= np.linalg.norm(r)
    return r, np.cross(d, r), d


def rasterize(model, az, el, window, res, color=False, light=None, visible=None):
    """Orthographic raster of every mesh. coverage (res,res) bool of opaque-after-alpha-test
    fragments; with color=True a depth-tested rgb image instead."""
    r, u, d = view_basis(az, el)
    cx, cy, half = window
    cov = np.zeros((res, res), bool)
    zb = np.full((res, res), np.inf)
    img = np.zeros((res, res, 3), np.float32)
    ids = np.full((res, res), -1, np.int64) if visible is not None else None
    for mi, m in enumerate(model['meshes']):
        P = m['attrs']['POSITION']
        t = m['idx'].reshape(-1, 3)
        sx = (P @ r - cx) / half * .5 * res + res / 2
        sy = res / 2 - (P @ u - cy) / half * .5 * res
        z = P @ d
        tx, ty, tz = sx[t], sy[t], z[t]
        uv = m['attrs']['TEXCOORD_0'][t]
        tex = model['tex'][m['tex']]
        nrm = m['attrs']['NORMAL'][t].mean(1)
        shade = .4 + .6 * np.abs(nrm @ (light if light is not None else d)) if color else None
        x0, x1 = np.floor(tx.min(1)).astype(int).clip(0, res - 1), np.ceil(tx.max(1)).astype(int).clip(0, res - 1)
        y0, y1 = np.floor(ty.min(1)).astype(int).clip(0, res - 1), np.ceil(ty.max(1)).astype(int).clip(0, res - 1)
        vis = (tx.max(1) >= 0) & (tx.min(1) <= res) & (ty.max(1) >= 0) & (ty.min(1) <= res)
        size = np.maximum(x1 - x0, y1 - y0) + 1
        bucket = np.ceil(np.log2(size)).astype(int)
        for b in np.unique(bucket[vis]):
            sel = np.nonzero(vis & (bucket == b))[0]
            B = 1 << b
            step = max(1, 2_000_000 // (B * B))
            og = np.stack(np.meshgrid(np.arange(B), np.arange(B)), -1).reshape(-1, 2)
            for s0 in range(0, len(sel), step):
                ii = sel[s0:s0 + step]
                px = x0[ii][:, None] + og[None, :, 0]
                py = y0[ii][:, None] + og[None, :, 1]
                fx, fy = px + .5, py + .5
                ax, ay, bx, by, cx_, cy_ = (tx[ii, 0, None], ty[ii, 0, None], tx[ii, 1, None], ty[ii, 1, None], tx[ii, 2, None], ty[ii, 2, None])
                den = (by - cy_) * (ax - cx_) + (cx_ - bx) * (ay - cy_)
                den = np.where(np.abs(den) < 1e-12, 1e-12, den)
                w0 = ((by - cy_) * (fx - cx_) + (cx_ - bx) * (fy - cy_)) / den
                w1 = ((cy_ - ay) * (fx - cx_) + (ax - cx_) * (fy - cy_)) / den
                w2 = 1 - w0 - w1
                ok = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (px <= x1[ii][:, None]) & (py <= y1[ii][:, None])
                # sub-pixel triangles: sample the centroid pixel so thin LOD0 geometry still counts
                none = ~ok.any(1)
                if none.any():
                    cxp = np.clip(np.floor((ax + bx + cx_)[none, 0] / 3), 0, res - 1)
                    cyp = np.clip(np.floor((ay + by + cy_)[none, 0] / 3), 0, res - 1)
                    px[none, 0], py[none, 0] = cxp, cyp
                    w0[none, 0] = w1[none, 0] = w2[none, 0] = 1 / 3
                    ok[none, 0] = True
                ti, ci = np.nonzero(ok)
                if not len(ti):
                    continue
                g = ii[ti]
                W = np.stack([w0[ti, ci], w1[ti, ci], w2[ti, ci]], 1)
                fuv = np.einsum('fk,fkj->fj', W, uv[g])
                al = alpha_at(tex, fuv) if m['cls'] == 'leaf' else np.ones(len(g), bool)
                X, Y = px[ti, ci][al], py[ti, ci][al]
                if not color and visible is None:
                    cov[Y, X] = True
                    continue
                zf = np.einsum('fk,fk->f', W, tz[g])[al]
                gi = g[al]
                keep = np.argsort(zf, kind='stable')  # far first; ties consistently retain the last card
                X, Y, zf, gi = X[keep], Y[keep], zf[keep], gi[keep]
                fu = fuv[al][keep]
                ph = (-zf) < zb[Y, X]
                X, Y, zf, gi, fu = X[ph], Y[ph], zf[ph], gi[ph], fu[ph]
                if visible is not None:
                    ids[Y, X] = (np.int64(mi) << 32) | (gi // 2) if m['cls'] == 'leaf' else -1
                    zb[Y, X] = -zf
                    continue
                h, w = tex.shape[:2]
                rgb = tex[(fu[:, 1] % 1 * h).astype(int).clip(0, h - 1), (fu[:, 0] % 1 * w).astype(int).clip(0, w - 1), :3] / 255.
                img[Y, X] = rgb * shade[gi][:, None]
                zb[Y, X] = -zf
    if visible is not None:
        winners = np.unique(ids[ids >= 0])
        for mi, keep in enumerate(visible):
            keep[(winners[winners >> 32 == mi] & 0xffffffff).astype(np.int64)] = True
    return img if color else cov


VIEWS = [(k * 45., 10. if k % 2 == 0 else 45.) for k in range(8)] + [(0., 90.)]


def prune_unseen_cards(model, res=256):
    """Keep LOD2 cards visible in any cook view, without enlarging or moving survivors.

    This is a sampled visibility approximation; the in-engine look comparison remains required.
    The old area-weighted subset spends hundreds of thousands of cards inside the crown.
    """
    visible = [np.zeros(len(m['idx']) // 6, bool) if m['cls'] == 'leaf' else np.zeros(0, bool)
               for m in model['meshes']]
    c, h = window_of(model)
    for az, el in VIEWS:
        rasterize(model, az, el, view_window(c, h, az, el), res, visible=visible)
    meshes = []
    for m, keep in zip(model['meshes'], visible):
        if m['cls'] == 'leaf':
            if not keep.any():
                keep[0] = True  # retain the authored shape node even if wholly occluded
            attrs, idx = compact(m['attrs'], m['idx'].reshape(-1, 6)[keep].ravel())
            m = dict(m, attrs=attrs, idx=idx)
        meshes.append(m)
    return dict(model, meshes=meshes)


def window_of(model):
    P = np.concatenate([m['attrs']['POSITION'] for m in model['meshes']])
    c, h = (P.min(0) + P.max(0)) / 2, np.linalg.norm(P.max(0) - P.min(0)) / 2 * 1.03
    return c, h


def cap_crown_coverage(model, nearer, res=256):
    """Bound enlarged coarse cards by the nearer crown, retaining the card budget."""
    c, h = window_of(nearer)
    windows = [view_window(c, h, az, el) for az, el in VIEWS]
    limits = [rasterize(nearer, az, el, win, res).sum()
              for (az, el), win in zip(VIEWS, windows)]
    leaves = [m for m in model['meshes'] if m['cls'] == 'leaf']
    original = [m['attrs']['POSITION'].reshape(-1, 4, 3).copy() for m in leaves]
    centers = [p.mean(1, keepdims=True) for p in original]
    scale = 1.
    for _ in range(32):
        coverage = [rasterize(model, az, el, win, res).sum()
                    for (az, el), win in zip(VIEWS, windows)]
        if all(a <= b for a, b in zip(coverage, limits)):
            return scale
        scale *= .96
        for m, p, center in zip(leaves, original, centers):
            m['attrs']['POSITION'] = (center + scale * (p - center)).reshape(-1, 3)
    raise ValueError('coarse crown still exceeds nearer coverage')


def view_window(c, h, az, el):
    r, u, _ = view_basis(az, el)
    return float(c @ r), float(c @ u), float(h)


def lowpass(cov, block=8):
    n = cov.shape[0] // block
    return cov[:n * block, :n * block].reshape(n, block, n, block).mean((1, 3))


def coverage_error(model0, model, res=256, cache={}):
    c, h = window_of(model0)
    errs = []
    for k, (az, el) in enumerate(VIEWS):
        win = view_window(c, h, az, el)
        key = (id(model0), k)
        if key not in cache:
            cache[key] = lowpass(rasterize(model0, az, el, win, res))
        d0, d1 = cache[key], lowpass(rasterize(model, az, el, win, res))
        errs.append(float(np.abs(d1 - d0).sum() / max(d0.sum(), 1e-9)))
    return dict(mean=float(np.mean(errs)), max=float(np.max(errs)), per_view=[round(e, 4) for e in errs])


# --------------------------------------------------------------------------- write
def write_level(src_model, meshes, path):
    doc = copy.deepcopy(src_model['doc'])
    doc['accessors'], doc['bufferViews'], data = [], [], bytearray()

    def view(raw, target=None):
        data.extend(b'\0' * (-len(data) % 4))
        bv = dict(buffer=0, byteOffset=len(data), byteLength=len(raw))
        if target:
            bv['target'] = target
        doc['bufferViews'].append(bv)
        data.extend(raw)
        return len(doc['bufferViews']) - 1
    by_node = {m['node']: m for m in meshes}
    for ni, node in enumerate(src_model['doc']['nodes']):
        if 'mesh' not in node:
            continue
        m = by_node[ni]
        prim = doc['meshes'][node['mesh']]['primitives'][0]
        for key, arr in m['attrs'].items():
            arr = np.ascontiguousarray(arr, np.float32)
            acc = dict(bufferView=view(arr.tobytes(), 34962), componentType=5126, count=len(arr), type={1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4'}[arr.shape[1]])
            if key == 'POSITION':
                acc.update(min=arr.min(0).tolist(), max=arr.max(0).tolist())
            doc['accessors'].append(acc)
            prim['attributes'][key] = len(doc['accessors']) - 1
        doc['accessors'].append(dict(bufferView=view(m['idx'].astype('<u4').tobytes(), 34963), componentType=5125, count=len(m['idx']), type='SCALAR'))
        prim['indices'] = len(doc['accessors']) - 1
    write_glb(path, doc, data)


# --------------------------------------------------------------------------- cook
def cook(source, texdir, outdir, sheet=False, log=print):
    source, outdir = Path(source), Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    stem = source.name.replace('lod0', '{}')
    m0 = load(source, texdir)
    base = m0['meshes']
    report = dict(source=str(source), components=component_report(m0), levels={}, gates={})
    c, h = window_of(m0)
    diag = 2 * h / 1.03
    t0 = tris(base)
    leaves = [i for i, m in enumerate(base) if m['cls'] == 'leaf']
    barks = [i for i, m in enumerate(base) if m['cls'] == 'bark']
    cards = {i: card_hulls(base[i], m0['tex'][base[i]['tex']]) for i in leaves}
    rng = np.random.default_rng(1114)
    # Area already determines survivor size; weighting selection by it again biases crown colour.
    order = {i: rng.permutation(cards[i]['nc']) for i in leaves}
    ncards = sum(cards[i]['nc'] for i in leaves)
    bark0 = tris(base, 'bark')
    lvl = {0: base}
    # leaf plan: cards kept per level as a global fraction (nested: same order prefix)
    f3 = min(1., LEAF_LOD3_SHARE * t0 / 2 / ncards)
    ffar = min(1., LEAF_FAR_TRIS / 2 / ncards)
    fracs = (1., LEAF_FRAC[1], LEAF_FRAC[2], f3, ffar)
    prev = {i: dedupe_tris(base[i]['attrs']['POSITION'], base[i]['idx']) for i in barks}
    report['bark_duplicate_tri_fraction'] = {i: 1 - len(prev[i]) / len(base[i]['idx']) for i in barks}
    for L in range(1, 5):
        out = []
        for i, m in enumerate(base):
            if m['cls'] == 'leaf':
                attrs, idx, scale = leaf_level(m, cards[i], order[i], fracs[L])
                out.append(dict(m, attrs=attrs, idx=idx, scale=scale))
            else:
                tgt = max(4, int(len(m['idx']) // 3 * BARK_RATIO[L])) if L < 4 else max(4, int(BARK_FAR_TRIS * len(m['idx']) / 3 / bark0))
                err = BARK_ERR[L]
                idx, slp = simplify_bark(m['attrs'], prev[i], tgt, err * diag, L <= 2, L >= 2)
                while L == 4 and len(idx) // 3 > 1.5 * tgt and err < .4:   # far tier: relax the cap until near target
                    err *= 1.5
                    idx, slp = simplify_bark(m['attrs'], prev[i], tgt, err * diag, L <= 2, L >= 2)
                m = dict(m, far_err=err if L == 4 else 0, sloppy=slp)
                prev[i] = idx
                attrs, cidx = compact(m['attrs'], idx)
                out.append(dict(m, attrs=attrs, idx=cidx))
        lvl[L] = out
    # write + read back (the gates measure the files, not the in-memory arrays)
    models = {0: m0}
    shutil.copyfile(source, outdir / stem.format('lod0'))
    for L in range(1, 5):
        p = outdir / stem.format(LEVELS[L])
        if L >= 3:
            for i in leaves:
                keep = len(models[L - 1]['meshes'][i]['idx']) // 6
                if len(lvl[L][i]['idx']) // 6 > keep:
                    if keep:
                        attrs, idx, scale = leaf_level(base[i], cards[i], order[i], n_keep=keep)
                        lvl[L][i] = dict(lvl[L][i], attrs=attrs, idx=idx, scale=scale)
                    else:
                        lvl[L][i] = dict(lvl[L][i], attrs={k: v[:0] for k, v in base[i]['attrs'].items()}, idx=base[i]['idx'][:0])
        write_level(m0, lvl[L], p)
        models[L] = load(p, texdir)
        if L == 2:
            pruned = prune_unseen_cards(models[L])
            write_level(m0, pruned['meshes'], p)
            models[L] = load(p, texdir)
        if L >= 3:
            cap_crown_coverage(models[L], models[L - 1])
            write_level(m0, models[L]['meshes'], p)
            models[L] = load(p, texdir)
    nodes0 = sum('mesh' in n for n in m0['doc']['nodes'])
    b0 = geometry_bytes(base)
    fails = []
    prevt = None
    for L in range(5):
        ms = models[L]['meshes']
        row = dict(level=LEVELS[L], tris=tris(ms), bark_tris=tris(ms, 'bark'), leaf_tris=tris(ms, 'leaf'),
                   geometry_bytes=geometry_bytes(ms), file_bytes=(outdir / stem.format(LEVELS[L])).stat().st_size,
                   shape_nodes=len(ms), per_node_tris=[len(m['idx']) // 3 for m in ms])
        row['coverage_error'] = coverage_error(m0, models[L]) if L else dict(mean=0., max=0., per_view=[])
        if L and row['coverage_error']['mean'] > GATE_COVERAGE[L]:
            fails.append(f'coverage {LEVELS[L]} {row["coverage_error"]["mean"]:.3f} > {GATE_COVERAGE[L]}')
        if row['shape_nodes'] != nodes0:
            fails.append(f'shape nodes {LEVELS[L]} {row["shape_nodes"]} != {nodes0}')
        if prevt is not None:
            if row['tris'] >= prevt['tris']:
                fails.append(f'chain not monotonic at {LEVELS[L]}')
            if any(a > b for a, b in zip(row['per_node_tris'], prevt['per_node_tris'])) and L > 1:
                fails.append(f'per-node tris grow at {LEVELS[L]}')
        prevt = row
        report['levels'][LEVELS[L]] = row
        log(json.dumps({k: v for k, v in row.items() if k != 'per_node_tris'}))
    lv = report['levels']
    if lv['lod3']['tris'] > GATE_LOD3 * t0:
        fails.append(f'lod3 {lv["lod3"]["tris"]} > {GATE_LOD3 * t0:.0f}')
    if lv['far']['tris'] > GATE_FAR:
        fails.append(f'far {lv["far"]["tris"]} > {GATE_FAR}')
    total = sum(lv[k]['geometry_bytes'] for k in LEVELS)
    report['bytes'] = dict(lod0=b0, all_levels=total, ratio=total / b0)
    if total > GATE_BYTES * b0:
        fails.append(f'bytes {total / b0:.3f}x > {GATE_BYTES}')
    report['leaf_scales'] = {LEVELS[L]: [round(m['scale'], 2) for m in lvl[L] if 'scale' in m] for L in range(1, 5)}
    report['sloppy_bark_nodes'] = {LEVELS[L]: [m['node'] for m in lvl[L] if m.get('sloppy')] for L in range(1, 5)}
    report['far_bark_err_cap_frac_of_diag'] = max((m.get('far_err', 0) for m in lvl[4]), default=0)
    report['uv_fit_max_residual'] = max(cards[i]['uv_fit_max_residual'] for i in leaves)
    report['gates'] = dict(failed=fails, passed=not fails, coverage_bound=GATE_COVERAGE)
    (outdir / (stem.format('report').replace('.glb', '.json'))).write_text(json.dumps(report, indent=1))
    if sheet:
        contact_sheet(models, outdir / stem.format('sheet').replace('.glb', '.png'))
    return report


def contact_sheet(models, path, tile=320):
    c, h = window_of(models[0])
    views = [(30., 15.), (150., 15.), (0., 90.)]
    sheet = np.full((tile * len(views), tile * 5, 3), .5, np.float32)
    for r_, (az, el) in enumerate(views):
        win = view_window(c, h, az, el)
        light = np.array([.4, .8, .4]) / np.linalg.norm([.4, .8, .4])
        for L in range(5):
            img = rasterize(models[L], az, el, win, tile, color=True, light=light)
            blank = img.sum(2) == 0
            img[blank] = .5
            sheet[r_ * tile:(r_ + 1) * tile, L * tile:(L + 1) * tile] = img
    Image.fromarray((sheet.clip(0, 1) * 255).astype(np.uint8)).save(path)


if __name__ == '__main__':
    rep = cook(sys.argv[1], sys.argv[2], sys.argv[3], sheet='--sheet' in sys.argv)
    for f in rep['gates']['failed']:
        print('GATE FAIL:', f)
    print('GATES', 'PASS' if rep['gates']['passed'] else 'FAIL')
    sys.exit(0 if rep['gates']['passed'] else 1)
