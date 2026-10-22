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
