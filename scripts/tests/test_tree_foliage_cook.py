import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import tree_foliage_cook as c  # noqa: E402


def _leaf_mesh(n=40, seed=3):
    """n tessellated quad 'leaves' (4x4 grid = 32 tris) scattered in a box; UV = full window."""
    rng = np.random.default_rng(seed)
    pos, uv, idx = [], [], []
    g = np.stack(np.meshgrid(np.linspace(0, 1, 5), np.linspace(0, 1, 5)), -1).reshape(-1, 2)
    quad = []
    for y in range(4):
        for x in range(4):
            a = y * 5 + x
            quad += [a, a + 1, a + 5, a + 1, a + 6, a + 5]
    for i in range(n):
        o = rng.random(3) * 20
        pos.append(np.c_[g[:, 0] * 2 + o[0], g[:, 1] * 3 + o[1], np.full(25, o[2])])
        uv.append(g)
        idx.append(np.array(quad) + 25 * i)
    nrm = np.tile([0., 0., 1.], (25 * n, 1))
    return dict(attrs={'POSITION': np.concatenate(pos).astype(np.float32), 'NORMAL': nrm.astype(np.float32),
                       'TEXCOORD_0': np.concatenate(uv).astype(np.float32)},
                idx=np.concatenate(idx).astype(np.uint32), cls='leaf', tex=0, node=0)


TEX = np.full((8, 8, 4), 255, np.uint8)


def test_hull_collapses_each_leaf_to_two_triangles_with_exact_uv():
    m = _leaf_mesh()
    cards = c.card_hulls(m, TEX)
    assert cards['nc'] == 40 and cards['uv_fit_max_residual'] < 1e-5
    attrs, idx, s = c.leaf_level(m, cards, np.arange(40), n_keep=40)
    assert len(idx) // 3 == 80 and s == 1.0
    assert np.allclose(cards['weight'], 6, rtol=1e-4)


def test_nested_subsets_preserve_visible_area():
    m = _leaf_mesh()
    cards = c.card_hulls(m, TEX)
    order = np.argsort(-cards['weight'])
    prev = None
    for k in (40, 20, 10, 5):
        attrs, idx, s = c.leaf_level(m, cards, order, n_keep=k)
        a = attrs['POSITION'].reshape(k, 4, 3)
        area = np.linalg.norm(np.cross(a[:, 1] - a[:, 0], a[:, 3] - a[:, 0]), axis=1).sum()
        assert abs(area - cards['weight'].sum()) / cards['weight'].sum() < 1e-4
        assert prev is None or len(idx) < prev
        prev = len(idx)


def test_bark_chain_is_monotonic_and_respects_cap():
    n = 40
    g = np.mgrid[0:n, 0:n].reshape(2, -1).T.astype(np.float32)
    pos = np.c_[g, np.sin(g[:, 0] / 6) * 3].astype(np.float32)
    idx = []
    for y in range(n - 1):
        for x in range(n - 1):
            a = y * n + x
            idx += [a, a + 1, a + n, a + 1, a + n + 1, a + n]
    attrs = {'POSITION': pos, 'NORMAL': np.tile([0, 0, 1.], (len(pos), 1)).astype(np.float32), 'TEXCOORD_0': g / n}
    idx = np.array(idx, np.uint32)
    counts = [len(idx)]
    for tgt, err in ((600, .02), (150, .05), (30, .1)):
        idx, _ = c.simplify_bark(attrs, idx, tgt, err * n)
        counts.append(len(idx))
    assert counts == sorted(counts, reverse=True) and len(set(counts)) == 4 and counts[-1] >= 6


def test_coverage_error_zero_for_identical_and_positive_for_thinned():
    m = _leaf_mesh()
    model = dict(doc={}, meshes=[m], tex={0: TEX})
    assert c.coverage_error(model, model, res=64, cache={})['mean'] == 0.
    cards = c.card_hulls(m, TEX)
    attrs, idx, _ = c.leaf_level(m, cards, np.arange(40), n_keep=4)
    thin = dict(doc={}, meshes=[dict(m, attrs=attrs, idx=idx)], tex={0: TEX})
    assert c.coverage_error(model, thin, res=64, cache={})['mean'] > 0
