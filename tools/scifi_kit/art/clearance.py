"""Mechanical-plausibility check for intent-pass assets (owner 2026-10-24: ladders are a path).

Strict, geometric, no contour fitting:
  1. pass-through: the centreline of every ladder stile/rung/cage hoop, stair stringer/handrail and
     railing rail must not cross any other part (its own ladder/stair/railing excepted; contact within
     8 cm of a member's end is its footing or landing and is allowed);
  2. path: each ladder/stair bottom stands on ground or a walkable surface, each top lands on a
     walkable surface reached by stepping toward the structure, through an OPEN railing;
  3. stand-off: wall/shell ladders keep their rungs 0.12-0.30 m off the climbed surface;
  4. reach: every platform is the landing (or start) of some ladder or stair.
Runs on the game carrier (what ships): pure numpy, no Blender.
"""
import numpy as np

EPS_END = .08


def _tris(k):
    V0, V1, V2, own = [], [], [], []
    for i, pc in enumerate(k.pieces):
        if pc['role'] == 'detail' or not pc['f']:
            continue
        v = np.asarray(pc['v'], float)
        for f in pc['f']:
            for j in range(1, len(f) - 1):
                V0.append(v[f[0]])
                V1.append(v[f[j]])
                V2.append(v[f[j + 1]])
                own.append(i)
    return np.array(V0), np.array(V1), np.array(V2), np.array(own)


class Scene:
    def __init__(self, k):
        self.k = k
        self.V0, self.V1, self.V2, self.own = _tris(k)
        self.E1, self.E2 = self.V1 - self.V0, self.V2 - self.V0
        self.lo = np.minimum(np.minimum(self.V0, self.V1), self.V2)
        self.hi = np.maximum(np.maximum(self.V0, self.V1), self.V2)
        self.aid = np.array([k.pieces[i].get('aid', -1) for i in self.own])
        self.walk = np.array([bool(k.pieces[i].get('walk')) for i in self.own])

    def hits(self, p, q, skip_aid=None):
        """Triangles crossed by segment p-q: (tri index, t along segment)."""
        p, q = np.asarray(p, float), np.asarray(q, float)
        slo, shi = np.minimum(p, q) - 1e-4, np.maximum(p, q) + 1e-4
        m = np.all(self.hi >= slo, 1) & np.all(self.lo <= shi, 1)
        if skip_aid is not None:
            m &= self.aid != skip_aid
        idx = np.nonzero(m)[0]
        if not len(idx):
            return []
        d = q - p
        e1, e2, v0 = self.E1[idx], self.E2[idx], self.V0[idx]
        h = np.cross(d, e2)
        a = np.einsum('ij,ij->i', e1, h)
        ok = np.abs(a) > 1e-12
        f = np.where(ok, 1.0 / np.where(ok, a, 1), 0)
        s = p - v0
        u = f * np.einsum('ij,ij->i', s, h)
        qv = np.cross(s, e1)
        v = f * (qv @ d)
        t = f * np.einsum('ij,ij->i', e2, qv)
        good = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t >= 0) & (t <= 1)
        return list(zip(idx[good], t[good]))

    def piece(self, tri):
        return self.k.pieces[self.own[tri]]

    def first_down(self, p, depth, skip_aid=None):
        hs = self.hits(p, (p[0], p[1], p[2] - depth), skip_aid)
        return min(hs, key=lambda x: x[1]) if hs else None


def describe(pc):
    return f"{pc.get('access') or pc['role']}:{pc['m']}"


def check(k):
    sc = Scene(k)
    bad = []
    # 1. pass-through
    for pc in k.pieces:
        if pc.get('access') not in ('ladder', 'stair', 'rail') or not pc.get('axis'):
            continue
        ax = [np.asarray(a, float) for a in pc['axis']]
        total = sum(np.linalg.norm(b - a) for a, b in zip(ax, ax[1:]))
        run = 0.0
        for a, b in zip(ax, ax[1:]):
            L = np.linalg.norm(b - a)
            for tri, t in sc.hits(a, b, skip_aid=pc['aid']):
                s = run + t * L
                if s < EPS_END or s > total - EPS_END:
                    continue
                other = sc.piece(tri)
                pt = a + (b - a) * t
                bad.append(f"{pc['access']} {pc['aid']} passes through {describe(other)} at "
                           f"({pt[0]:.1f}, {pt[1]:.1f}, {pt[2]:.1f})")
                break
            run += L
    # 2-3. paths
    reached = set()
    for e in k.meta.get('access', []):
        tag = f"{e['kind']} {e['aid']} ({e['name']})"
        b, t, st = np.asarray(e['bottom'], float), np.asarray(e['top'], float), np.asarray(e['step'], float)
        if b[2] > .05:
            h = sc.first_down(b + (0, 0, .3), .55, e['aid'])
            if not h or not sc.walk[h[0]]:
                bad.append(f"{tag}: bottom does not stand on a walkable surface")
            elif sc.piece(h[0]).get('platform'):
                reached.add(sc.piece(h[0])['platform'])
        reach = (e.get('off', 0) + .6) if e['kind'] == 'ladder' else .45     # one stride past the climbed face
        land = t + st * reach + (0, 0, .6)
        h = sc.first_down(land, 1.0, e['aid'])
        if not h or not sc.walk[h[0]]:
            bad.append(f"{tag}: top does not land on a walkable surface" + (f" (hits {describe(sc.piece(h[0]))})" if h else ''))
        else:
            if sc.piece(h[0]).get('platform'):
                reached.add(sc.piece(h[0])['platform'])
            if abs(.6 - h[1]) > .45:
                bad.append(f"{tag}: landing is {.6 - h[1]:+.2f} m off the top level")
        for hh in (.5, .95):
            p0 = t + (0, 0, hh) - st * (.05 if e['kind'] == 'ladder' else .3)
            for tri, _ in sc.hits(p0, t + st * reach + (0, 0, hh), e['aid']):
                if not sc.walk[tri]:
                    bad.append(f"{tag}: step-off at the top is blocked by {describe(sc.piece(tri))}")
                    break
            else:
                continue
            break
        if e['kind'] == 'ladder' and e.get('standoff'):
            mid = (b + t) / 2
            hs = sc.hits(mid, mid + st * .6, e['aid'])
            d = min(x[1] for x in hs) * .6 if hs else None
            if d is None or not (.12 <= d <= .30):
                bad.append(f"{tag}: rungs stand {'clear of anything' if d is None else f'{d:.2f} m'} off the climbed surface (want 0.12-0.30)")
    # 4. reach
    for pf in k.meta.get('platforms', []):
        if pf['pid'] not in reached:
            bad.append(f"platform {pf['pid']} ({pf['kind']}) is not reached by any ladder or stair")
    bad += purpose(k, sc)
    return bad


# service destinations: the platform exists to reach equipment (valves, flues, rims, heads, gantries, masts)
PURPOSE = ('gantry', 'service', 'head', 'searchlight', 'siren', 'mast', 'deck', 'cab', 'gun', 'catwalk', 'roof', 'valve', 'rim', 'flue',
           'column', 'clarifier', 'walk')


def _doors(k):
    out = []
    for pc in k.pieces:
        if pc.get('door') or pc.get('feature') in ('door', 'rollup', 'gate') or pc['m'] == 'paint.door':
            v = np.asarray(pc['v'], float)
            out.append((pc, v.min(0), v.max(0)))
    return out


def _tris_in_box(V0, V1, V2, lo, hi):
    """True when any triangle really intersects the box (separating-axis test), so a round tank or a cone whose
    bounding box only grazes a door zone does not count as standing in it."""
    c = (lo + hi) / 2
    e = (hi - lo) / 2
    a, b, d = V0 - c, V1 - c, V2 - c
    ok = np.ones(len(a), bool)
    for i in range(3):                                   # box face normals
        mn = np.minimum(np.minimum(a[:, i], b[:, i]), d[:, i])
        mx = np.maximum(np.maximum(a[:, i], b[:, i]), d[:, i])
        ok &= (mn <= e[i]) & (mx >= -e[i])
    f = [b - a, d - b, a - d]
    n = np.cross(f[0], f[1])                             # triangle normal
    r = np.abs(n) @ e
    s = np.einsum('ij,ij->i', n, a)
    ok &= np.abs(s) <= r + 1e-9
    for ei in range(3):                                  # edge x axis cross products
        u = np.zeros(3)
        u[ei] = 1
        for fj in f:
            ax_ = np.cross(np.broadcast_to(u, fj.shape), fj)
            pa, pb, pd = (np.einsum('ij,ij->i', ax_, t) for t in (a, b, d))
            rr = np.abs(ax_) @ e
            ok &= ~((np.minimum(np.minimum(pa, pb), pd) > rr) | (np.maximum(np.maximum(pa, pb), pd) < -rr))
    return bool(ok.any())


DOOR_CLEAR = 2.0          # m of clear approach in front of every door (owner round 6)
FLOAT_GAP = .15           # m: a part standing further than this off everything grounded is floating


def _closest_dist(P, A, B, C):
    """Distance from each point in P (n,3) to the nearest of the triangles (A, B, C) (m,3 each)."""
    best = np.full(len(P), np.inf)
    for i0 in range(0, len(A), 4096):
        a, b, c = A[i0:i0 + 4096][None], B[i0:i0 + 4096][None], C[i0:i0 + 4096][None]
        p = P[:, None]
        ab, ac, ap = b - a, c - a, p - a
        d1, d2 = (ab * ap).sum(-1), (ac * ap).sum(-1)
        bp = p - b
        d3, d4 = (ab * bp).sum(-1), (ac * bp).sum(-1)
        cp = p - c
        d5, d6 = (ab * cp).sum(-1), (ac * cp).sum(-1)
        va = d3 * d6 - d5 * d4
        vb = d5 * d2 - d1 * d6
        vc = d1 * d4 - d3 * d2
        den = np.where(np.abs(va + vb + vc) < 1e-12, 1e-12, va + vb + vc)
        v = vb / den
        w = vc / den
        q = a + ab * v[..., None] + ac * w[..., None]                 # interior projection
        # vertex / edge regions
        q = np.where(((d1 <= 0) & (d2 <= 0))[..., None], a, q)
        q = np.where(((d3 >= 0) & (d4 <= d3))[..., None], b, q)
        q = np.where(((d6 >= 0) & (d5 <= d6))[..., None], c, q)
        t_ab = np.clip(d1 / np.where(np.abs(d1 - d3) < 1e-12, 1e-12, d1 - d3), 0, 1)
        q = np.where(((vc <= 0) & (d1 >= 0) & (d3 <= 0))[..., None], a + ab * t_ab[..., None], q)
        t_ac = np.clip(d2 / np.where(np.abs(d2 - d6) < 1e-12, 1e-12, d2 - d6), 0, 1)
        q = np.where(((vb <= 0) & (d2 >= 0) & (d6 <= 0))[..., None], a + ac * t_ac[..., None], q)
        den_bc = (d4 - d3) + (d5 - d6)
        t_bc = np.clip((d4 - d3) / np.where(np.abs(den_bc) < 1e-12, 1e-12, den_bc), 0, 1)
        q = np.where(((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0))[..., None], b + (c - b) * t_bc[..., None], q)
        best = np.minimum(best, np.linalg.norm(p - q, axis=-1).min(1))
    return best


def attached(k, sc, glass_gap=.12, part_gap=.12):
    """Owner round 6 ("glass hanging off", "parts floating"): every glazing pane must sit on a frame or wall along its
    edges (each corner within 0.12 m of a non-glass surface), and every part must be connected through touching parts
    to the ground (bounding boxes within 0.12 m; a component that never reaches z < 0.15 is floating)."""
    bad = []
    pieces = [(i, pc, np.asarray(pc['v'], float)) for i, pc in enumerate(k.pieces) if pc['v'] and pc['f']]
    if not pieces:
        return bad
    solid = np.array([k.pieces[o]['m'] != 'glass' and k.pieces[o]['role'] != 'glass' for o in sc.own]) if len(sc.own) else np.array([], bool)
    A, B, C = sc.V0[solid], sc.V1[solid], sc.V2[solid]
    for i, pc, v in pieces:
        if pc['m'] != 'glass' and pc['role'] != 'glass':
            continue
        lo, hi = v.min(0), v.max(0)
        # the pane's own extreme vertices stand in for its corners (lofted / slanted glazing)
        ext = np.unique(np.r_[v[v[:, 0].argmin()][None], v[v[:, 0].argmax()][None], v[v[:, 1].argmin()][None],
                              v[v[:, 1].argmax()][None], v[v[:, 2].argmin()][None], v[v[:, 2].argmax()][None]], axis=0)
        near = np.all(sc.lo[solid] <= hi + 1.0, 1) & np.all(sc.hi[solid] >= lo - 1.0, 1) if len(A) else np.array([], bool)
        if not near.any():
            bad.append(f"glass at ({v[:, 0].mean():.1f}, {v[:, 1].mean():.1f}, {v[:, 2].mean():.1f}) is hanging: no frame or wall near it")
            continue
        sel = np.where(near)[0]
        d = _closest_dist(ext, A[sel], B[sel], C[sel])
        if (d > glass_gap).mean() > .34:
            bad.append(f"glass at ({v[:, 0].mean():.1f}, {v[:, 1].mean():.1f}, {v[:, 2].mean():.1f}) is hanging off: "
                       f"{int((d > glass_gap).sum())} of {len(d)} edge points are {d.max():.2f} m from any frame or wall")
    # connectivity to the ground through touching parts (union-find on padded bounding boxes)
    los = np.array([v.min(0) for _, _, v in pieces]) - part_gap / 2
    his = np.array([v.max(0) for _, _, v in pieces]) + part_gap / 2
    n = len(pieces)
    parent = list(range(n + 1))                      # n = ground

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a in range(n):
        if los[a][2] < .15:
            parent[find(a)] = find(n)
        hit = np.where(np.all(los <= his[a], 1) & np.all(his >= los[a], 1))[0]
        for b in hit:
            if b != a:
                ra, rb = find(a), find(b)
                if ra != rb:
                    parent[ra] = rb
    g = find(n)
    groups = {}
    for a in range(n):
        if find(a) != g:
            groups.setdefault(find(a), []).append(a)
    for members in groups.values():
        lo = los[members].min(0)
        hi = his[members].max(0)
        mats = sorted({pieces[a][1]['m'] for a in members})
        others = np.array([a for a in range(n) if find(a) != find(members[0])])
        gap = np.maximum(np.maximum(los[others] - hi, lo - his[others]), 0)
        dmin = np.linalg.norm(gap, axis=1).min() + part_gap      # raw gap between the parts' bounding boxes
        if dmin <= FLOAT_GAP:
            continue                                                 # a seam of a few cm is not visible
        bad.append(f"floating: {len(members)} part(s) [{', '.join(mats[:3])}] near ({(lo[0] + hi[0]) / 2:.1f}, "
                   f"{(lo[1] + hi[1]) / 2:.1f}, {lo[2]:.1f}) stand {dmin:.2f} m clear of anything that reaches the ground")
    return bad


def purpose(k, sc):
    """Owner 2026-10-25 ("stairs to nowhere, stairs blocking doors"): every access path must lead somewhere and
    leave the doors usable.
      a. a top landing on a platform needs another exit from that platform (a second ladder/stair, a door within
         0.3 m of its deck) or a service purpose (gantry, equipment head, mast platform); roofs count as destinations;
      b. a bottom at ground level needs an open approach on at least one side;
      c. nothing (stairs, ladders, legs, braces, any part) may stand in a door's 1.2 m swept zone."""
    bad = []
    doors = _doors(k)
    plats = {pf['pid']: pf for pf in k.meta.get('platforms', [])}
    deck = {}
    for pc in k.pieces:
        if pc.get('platform') and pc.get('walk'):
            v = np.asarray(pc['v'], float)
            lo, hi = deck.get(pc['platform'], (v.min(0), v.max(0)))
            deck[pc['platform']] = (np.minimum(lo, v.min(0)), np.maximum(hi, v.max(0)))
    touches = {}
    ends = []
    for e in k.meta.get('access', []):
        for end in ('bottom', 'top'):
            p = np.asarray(e[end], float)
            for pid, (lo, hi) in deck.items():
                if np.all(p[:2] >= lo[:2] - .6) and np.all(p[:2] <= hi[:2] + .6) and abs(p[2] - hi[2]) < .5:
                    touches.setdefault(pid, set()).add(e['aid'])
                    ends.append((e, end, pid))
    for e, end, pid in ends:
        if end != 'top':
            continue
        name = plats.get(pid, {}).get('kind', '')
        if any(w in name for w in PURPOSE) or len(touches.get(pid, ())) >= 2:
            continue
        lo, hi = deck[pid]
        if any(np.all(dl[:2] <= hi[:2] + .6) and np.all(dh[:2] >= lo[:2] - .6) and abs(dl[2] - hi[2]) < .6 for _, dl, dh in doors):
            continue
        bad.append(f"{e['kind']} {e['aid']} ({e['name']}): its top platform '{name}' leads nowhere (no door, no other exit, no service purpose)")
    for e in k.meta.get('access', []):
        b = np.asarray(e['bottom'], float)
        if b[2] > .3:
            continue
        st = np.asarray(e['step'], float)
        side = np.array([-st[1], st[0], 0.0])
        ok = False
        for d in (-st, side, -side):
            p0 = b + (0, 0, 1.0)
            if not [h for h in sc.hits(p0, p0 + d * 1.2, e['aid']) if not sc.walk[h[0]]]:
                ok = True
                break
        if not ok:
            bad.append(f"{e['kind']} {e['aid']} ({e['name']}): no open approach to its foot")
    # owner round 6 ("doors blocked"): the wall a door sits in is the mass right behind its leaf (within 0.7 m);
    # a door with mass that close on both sides is buried or walled in, and the open side must be clear for 2 m
    # (a hut, wall, stair or any part within 2 m in front of a door blocks it)
    def zone_blocker(pc, dl, dh, thin, sgn):
        zlo, zhi = dl.copy(), dh.copy()
        zlo[2] = dl[2] + .55                   # above sills, aprons, kerbs and a single step
        zhi[2] = min(dh[2] - .1, dl[2] + 2.2)
        if sgn > 0:                            # from 0.4 m in front of the leaf (its own ribs, portal and seams) to 2 m
            zlo[thin], zhi[thin] = dh[thin] + .4, dh[thin] + DOOR_CLEAR
        else:
            zlo[thin], zhi[thin] = dl[thin] - DOOR_CLEAR, dl[thin] - .4
        ax = 1 - thin
        zlo[ax] += .15
        zhi[ax] -= .15
        for qi, q in enumerate(k.pieces):
            if q is pc or q['role'] == 'detail' or q.get('walk') or q.get('door') or q.get('feature') or q['m'] in ('glass', 'paint.door'):
                continue
            w = np.asarray(q['v'], float)
            if np.all(w.max(0) > zlo) and np.all(w.min(0) < zhi):
                m = sc.own == qi
                inside = q['role'] == 'mass' and np.all((zlo + zhi) / 2 > w.min(0)) and np.all((zlo + zhi) / 2 < w.max(0))
                if not inside and not _tris_in_box(sc.V0[m], sc.V1[m], sc.V2[m], zlo, zhi):
                    continue
                return f"{q.get('access') or q['role']}:{q['m']} [{w.min(0)[0]:.1f}, {w.min(0)[1]:.1f}, {w.min(0)[2]:.1f}]..[{w.max(0)[0]:.1f}, {w.max(0)[1]:.1f}, {w.max(0)[2]:.1f}]"
        return None

    for pc, dl, dh in doors:
        ext = dh - dl
        thin = int(np.argmin(ext[:2]))
        if ext[thin] > .5 or dl[2] > 1.0 or ext[2] < 1.2:     # not a leaf you walk through (sills, kick strips)
            continue
        c = (dl + dh) / 2
        open_sides = []
        for sgn in (-1, 1):
            n = np.zeros(3)
            n[thin] = sgn
            p0 = c + n * (ext[thin] / 2 + .01)          # just off this face of the leaf
            near = [h for h in sc.hits(p0, p0 + n * .7) if k.pieces[sc.own[h[0]]]['role'] == 'mass']
            through = [h for h in sc.hits(p0, p0 + n * 80.0) if k.pieces[sc.own[h[0]]]['role'] == 'mass']
            if not near and len(through) % 2 == 0:       # odd crossings: this side of the leaf starts inside a solid
                open_sides.append(sgn)
        if not open_sides:
            bad.append(f"door at ({c[0]:.1f}, {c[1]:.1f}): walled in - solid mass within 0.7 m on both sides of the leaf")
            continue
        found = [zone_blocker(pc, dl, dh, thin, sgn) for sgn in open_sides]
        if all(found):
            bad.append(f"door at ({c[0]:.1f}, {c[1]:.1f}): its 2 m approach is blocked by {found[0]}")
    bad += attached(k, sc)
    seen, out = set(), []
    for x in bad:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out
