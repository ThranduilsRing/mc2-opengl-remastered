"""Engine blocking-cell check against the stock placement survey (E:/mc2-archive-2026-10/proofs/placement-survey).

The engine marks a pathing cell (128/3 WU) blocked when any eligible mesh vertex with local z >= 1 WU falls in it
(no polygon filling), after rotating by the placement angle and translating to the placement position. The survey
records, for every stock placement, the absolute intact cells. This module converts a mesh in the authored frame
(our 0.25 m/WU, bbox-centred, z up) into the engine frame, calibrated on the stock mesh itself (axis signs and the
pivot offset from the survey's mesh bounds), then reproduces the cell sets at the recorded placements.

  cells.check(name, remake_tris) -> dict(placements, stock_match, remake_match, mismatches[...])
"""
import functools
import json
import math
from pathlib import Path

import numpy as np

SURVEY = Path('E:/mc2-archive-2026-10/proofs/placement-survey')
TILE = 128.0
WU_PER_M = 4.0                       # authoring metres in this pipeline (stock json vertices_m) = WU / 4


@functools.lru_cache(maxsize=1)
def _foot():
    return json.load(open(SURVEY / 'footprints.json'))['types']


@functools.lru_cache(maxsize=1)
def _placements():
    d = json.load(open(SURVEY / 'placements.json'))
    out = {}
    for mid, m in d['missions'].items():
        side = m['vertices_per_side']
        for p in m['placements']:
            out.setdefault(p['appearance'].lower(), []).append((side, p))
    return out


# never-placed variants that share a placed piece's massing and grid (intent_generic): test them at that piece's placements
SURVEY_AS = {}


def survey_type(name):
    name = SURVEY_AS.get(name, name)
    for k, v in _foot().items():
        if v.get('appearance', '').lower() == name.lower() or k.lower() == name.lower():
            return v
    return None


def _cell(x, y, side):
    half = np.float32(side * TILE / 2)
    c = (np.float32(x) + half) * np.float32(3 / TILE)
    r = (half - np.float32(y)) * np.float32(3 / TILE)
    return r.astype(np.int64), c.astype(np.int64)


def covered(P, rot, pos, side):
    a = math.radians(rot)
    c, s = np.float32(math.cos(a)), np.float32(math.sin(a))
    P = P.astype(np.float32)
    P = P[P[:, 2] >= 1.0]
    x = P[:, 0] * c - P[:, 1] * s + np.float32(pos[0])
    y = P[:, 0] * s + P[:, 1] * c + np.float32(pos[1])
    r, cc = _cell(x, y, side)
    m = (r >= 0) & (r < side * 3) & (cc >= 0) & (cc < side * 3)
    return set(zip(r[m].tolist(), cc[m].tolist()))


def to_engine(V, sx, sy, off):
    """Authored metres -> engine WU (mesh-local, pivot at origin)."""
    E = np.empty_like(V, dtype=np.float64)
    E[:, 0] = sx * V[:, 0] * WU_PER_M + off[0]
    E[:, 1] = sy * V[:, 1] * WU_PER_M + off[1]
    E[:, 2] = V[:, 2] * WU_PER_M
    return E


def sample(name, n=24):
    pl = _placements().get(SURVEY_AS.get(name, name).lower(), [])
    if not pl:
        return []
    by_rot = {}
    for side, p in pl:
        by_rot.setdefault(p['normalized_rotation_degrees'], []).append((side, p))
    out = []
    for rot, lst in sorted(by_rot.items()):
        step = max(1, len(lst) // max(1, n // len(by_rot)))
        out += lst[::step][:max(1, n // len(by_rot))]
    return out


@functools.lru_cache(maxsize=None)
def calibrate(name):
    """Pick axis signs and pivot offset so the STOCK mesh reproduces the recorded stock cells; -> (sx, sy, off, rate)."""
    import topdown
    t = survey_type(name)
    pts = sample(name)
    if t is None or not pts:
        return None
    S = topdown.stock_tris(name).reshape(-1, 3)
    best = None
    for sx in (1, -1):
        for sy in (1, -1):
            # pivot offset: put the stock bbox where the survey's mesh bounds say (the survey bounds include the pivot)
            b = (t.get('mesh') or {}).get('intact', {}).get('bounds_wu')
            if b:
                off = ((b[0][0] + b[1][0]) / 2, (b[0][1] + b[1][1]) / 2)
            else:
                off = (0.0, 0.0)
            E = to_engine(S, sx, sy, off)
            ok = sum(covered(E, p['rotation_degrees'], p['position_wu'], side) == set(map(tuple, p['coverage_cells_rc']['intact']))
                     for side, p in pts)
            rate = ok / len(pts)
            if best is None or rate > best[3]:
                best = (sx, sy, off, rate)
    return best


@functools.lru_cache(maxsize=1)
def _runs():
    r = json.load(open(SURVEY / 'runs.json'))['types']
    return {k.lower(): v for k, v in r.items()}


def pitch_m(name):
    """Most common same-type pitch from the survey runs, in authoring metres (None when never seen in a run)."""
    t = _runs().get(SURVEY_AS.get(name, name).lower())
    h = (t or {}).get('same_type_pitch_wu_histogram') or []
    if not h:
        return None
    best = max(h, key=lambda e: e['count'])
    return best['value'] / WU_PER_M


def check(name, R, ref=None, rotations=None):
    """Remake cells vs the stock at sampled survey placements. The reference is the stock mesh reconstructed with
    the same rule (its vertex-only cells); the survey's recorded cells can add cells from engine helper nodes the
    visual mesh does not carry, reported as stock_match."""
    import topdown
    cal = calibrate(name)
    if cal is None:
        return dict(placements=0, note='no survey placements')
    sx, sy, off, srate = cal
    pts = [x for x in sample(name, 64) if rotations is None or x[1]['normalized_rotation_degrees'] in rotations][:24]
    if not pts:
        return dict(placements=0, note='no placements at these rotations')
    E = to_engine(R.reshape(-1, 3), sx, sy, off)
    ES = to_engine((topdown.stock_tris(name) if ref is None else ref).reshape(-1, 3), sx, sy, off)
    mism = []
    for side, p in pts:
        want = covered(ES, p['rotation_degrees'], p['position_wu'], side)
        got = covered(E, p['rotation_degrees'], p['position_wu'], side)
        if got != want:
            mism.append(dict(id=p['id'], rot=p['normalized_rotation_degrees'], extra=len(got - want), missing=len(want - got)))
    return dict(placements=len(pts), stock_match=round(srate, 3), remake_match=round(1 - len(mism) / len(pts), 3), mismatches=mism[:6])
