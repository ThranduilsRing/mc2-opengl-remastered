"""Pure-Python checks for the art-directed kit (no Blender)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'art'))
import assemble  # noqa: E402
import geo  # noqa: E402

CONDITIONS = ['pristine', 'worn', 'abandoned', 'destroyed']


def _valid(k):
    assert k.pieces
    for pc in k.pieces:
        n = len(pc['v'])
        assert all(0 <= i < n for f in pc['f'] for i in f)
        assert pc['m'].split('.')[0] in geo.LANG['material_classes'] or pc['m'].startswith(('veg.', 'lamp_red'))
        assert pc['role'] in ('mass', 'skin', 'frame', 'roof', 'glass', 'detail')


@pytest.mark.parametrize('name', list(geo.PRIMITIVES))
@pytest.mark.parametrize('cond', CONDITIONS)
def test_primitives_build(name, cond):
    k = geo.build(name, cond)
    _valid(k)
    # nothing in the kit sinks below the ground plane
    assert min(p[2] for pc in k.pieces for p in pc['v']) > -0.05
    if cond == 'destroyed':
        assert k.hits, 'destroyed state needs an explicit damage origin'
        assert not [pc for pc in k.pieces if pc.get('pane')], 'destroyed glazing is blown out'


@pytest.mark.parametrize('name', list(assemble.ASSEMBLIES))
def test_assemblies_build(name):
    for cond in ('pristine', 'destroyed'):
        k = assemble.build(name, cond)
        _valid(k)
        assert k.meta['palette'] in geo.LANG['palettes']


def test_abandoned_is_not_worn():
    w, a = geo.build('window_slit', 'worn'), geo.build('window_slit', 'abandoned')
    assert any(pc['m'].startswith('veg.') for pc in a.pieces)
    assert not any(pc['m'].startswith('veg.') for pc in w.pieces)
    cw, ca = geo.LANG['conditions']['worn'], geo.LANG['conditions']['abandoned']
    for key in ('edge_wear', 'dirt', 'streaks', 'fade', 'peel', 'moss'):
        assert ca[key] > cw[key]


def test_palettes_carry_stock_identity():
    pal = geo.LANG['palettes']
    q = pal['quonset_rust']['corr']
    assert q[0] > 2 * q[2], 'quonset corrugation must stay rust-brown'
    s = pal['barracks_sage']['second']
    assert s[1] > s[0] and s[1] > s[2], 'barracks inset panels must stay sage green'


def test_wall_modules_on_grid():
    sub = geo.G['sub_m']
    for w in (4.5, 9.0, 19.5):
        assert abs(w / sub - round(w / sub)) < 1e-9


# ---------------------------------------------------------------- art bar (round 3)
import drafts  # noqa: E402

STATES = ['pristine', 'worn', 'abandoned', 'partial', 'destroyed']


def _game(name, cond):
    geo.GAME_MESH = True
    try:
        return drafts.build(name, cond)
    finally:
        geo.GAME_MESH = False


def test_tarp_drapes_over_the_wood_everywhere():
    for game in (False, True):
        geo.GAME_MESH = game
        try:
            k = geo.Kit('t', 'canvas_timber')
            drafts.tarp_crates(k)
        finally:
            geo.GAME_MESH = False
        assert drafts.tarp_clearance(k) > .015, 'tarp vertex inside or touching a crate'
        assert len({p['tarp'] for p in k.pieces if p.get('tarp') is not None}) == 2


@pytest.mark.parametrize('name', list(drafts.DRAFTS))
@pytest.mark.parametrize('cond', STATES)
def test_high_and_game_damage_in_lockstep(name, cond):
    drafts.FIT_CACHE.clear()
    hi = drafts.build(name, cond)
    lo = _game(name, cond)
    _valid(hi)
    _valid(lo)
    assert sorted(p['uid'] for p in hi.pieces if 'uid' in p) == sorted(p['uid'] for p in lo.pieces if 'uid' in p)
    assert min(v[2] for p in hi.pieces for v in p['v']) > -0.05  # moss pads sit 2 cm into the soil


@pytest.mark.parametrize('name', list(drafts.DRAFTS))
def test_game_carrier_leaves_bevel_headroom(name):
    import bake_budget
    for cond in STATES:
        lo = _game(name, cond)
        tris = sum(sum(len(f) - 2 for f in p['f']) for p in lo.pieces)
        assert tris <= bake_budget.BUDGETS.get(name, 3000), (name, cond, tris)


def test_rubble_is_volumetric():
    for name in drafts.BUILDINGS:
        k = drafts.build(name, 'destroyed')
        top = max(v[2] for p in k.pieces for v in p['v'])
        assert top > .14 * drafts.SIZES[name][2], name
        assert any(p['role'] == 'frame' for p in k.pieces), 'rubble keeps a bent frame'


@pytest.mark.parametrize('name', list(drafts.batch2.DRAFTS))
def test_batch_two_authored_proportions_match_stock(name):
    drafts.FIT_CACHE.clear()
    k = drafts.build(name, 'pristine')
    assert all(.9 <= scale <= 1.1 for scale in k.meta['fit_scale_xyz'])


def test_destroyed_metal_barricade_is_flat():
    k = drafts.build('corrigatedmetalbarricade', 'destroyed')
    assert max(v[2] for p in k.pieces for v in p['v']) < .4
