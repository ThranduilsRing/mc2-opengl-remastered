"""Pure-Python tests of the archetype pipeline gates (no Blender): the review's attack cases are regression tests.

Every attack below passed the old validator; each must now FAIL with the expected `kind:` prefix, while the shipped proof assets pass.
"""
import copy
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

ART = Path(__file__).resolve().parents[1] / 'art'
sys.path.insert(0, str(ART))
analyze = pytest.importorskip('analyze')
if not analyze.STOCK.is_dir():
    pytest.skip('stock mesh dump not available on this machine', allow_module_level=True)
import archetypes  # noqa: E402
import arch_props  # noqa: E402
import batch  # noqa: E402
import contract  # noqa: E402
import validate  # noqa: E402

PROOF = ('barracks', 'civstoragea', 'tent', 'woodenfence', 'smallfueltank', '2civliving', 'industrialcomplex', 'greenhouse')
SHAPE_FIX = ('smallfueltank', '2civliving', 'industrialcomplex', 'greenhouse')     # run-1 remakes with the wrong gross shape
ASSETS = ART / 'assets'


@pytest.fixture(scope='module', autouse=True)
def _registry_after_intent():
    """Order independence (tooling-speed-2a): the first import of `intent` (any test that builds a stock asset through assemble.build)
    overwrites counts.CONTRACTS/NOTES/ORIGINS, wiping what archetypes.register installed at drafts import, so the proof assets then
    fail validate with 'counts: doors 0 vs 2'. Import intent first and re-register, whatever ran before this module (xdist worker order)."""
    import intent  # noqa: F401
    archetypes.register_all()
    yield


def params(name):
    return json.loads((ASSETS / f'{name}.params.json').read_text())


def run(name, edit):
    """Validate a mutated in-memory copy of the asset's params; always restore the registry afterwards."""
    p = params(name)
    edit(p)
    archetypes.register(p)
    try:
        return validate.validate(name, params=p)
    finally:
        archetypes.register(params(name))


def kinds(res):
    return sorted({f.split(':')[0] for f in res['failures']})


def gate(name, edit):
    p = params(name)
    edit(p)
    fails, _, _ = contract.pre_gate(name, p, deep=False)
    return sorted({f.split(':')[0] for f in fails}), fails


# ------------------------------------------------------------------ shipped assets and integrity
@pytest.mark.parametrize('name', PROOF)
def test_proof_assets_pass(name):
    res = validate.validate(name)
    assert res['ok'], res['failures']


def test_thresholds_are_hash_locked():
    assert contract.thresholds_intact() == []


@pytest.mark.parametrize('name', PROOF)
def test_analysis_is_the_stock_derivation(name):
    assert contract.analysis_intact(name) == []
    assert contract.shape_intact(name) == []


def test_shape_edit_is_detected(tmp_path, monkeypatch):
    src = json.loads((ASSETS / 'greenhouse.shape.json').read_text())
    src['features'] = []
    (tmp_path / 'greenhouse.shape.json').write_text(json.dumps(src))
    monkeypatch.setattr(analyze, 'ASSETS', tmp_path)
    assert contract.shape_intact('greenhouse')


# ------------------------------------------------------------------ gross shape from the stock mesh (run-1 regressions)
def test_stock_shapes_are_measured():
    s = {n: json.loads((ASSETS / f'{n}.shape.json').read_text()) for n in SHAPE_FIX}
    fp = s['smallfueltank']['footprint']
    assert (fp['kind'], fp['facets']) == ('circle', 6)          # owner rule: the era's 6 facets are a round tank, rebuilt round
    assert s['smallfueltank']['roof']['model'] == 'inset' and s['smallfueltank']['roof']['top'] < 8.5   # cone roof below the side cylinder
    assert any(f['shape'] == 'round' and f['base'] == 0 and f['top'] > 10 for f in s['smallfueltank']['features'])   # the oval side cylinder
    assert (s['2civliving']['footprint']['kind'], s['2civliving']['footprint']['n']) == ('rectilinear', 6)    # L plan
    assert s['industrialcomplex']['roof']['model'] == 'extrude' and s['industrialcomplex']['roof']['form'] == 'stepped'
    assert len([f for f in s['greenhouse']['features'] if f['kind'] == 'fan']) == 5     # five painted roof fans
    assert s['greenhouse']['roof']['model'] == 'extrude' and s['greenhouse']['roof']['eave'] < .5   # A-frame to the ground


def _old_shape(name):
    """The gross shape run 1 shipped: a full-height can without its side cylinder, a full square box, a square hip-roofed block,
    a greenhouse without fans."""
    import copy
    import shell
    S = copy.deepcopy(shell.load(name))
    w, d, h = S['size_m']
    rect = [[-w / 2, -d / 2], [w / 2, -d / 2], [w / 2, d / 2], [-w / 2, d / 2]]
    if name == 'smallfueltank':
        return None                      # no measured shape: the bbox-sized round can of run 1
    elif name == '2civliving':
        S['footprint'] = dict(kind='rect', n=4, poly=rect, edges=[rect[i] + rect[(i + 1) % 4] for i in range(4)], cells=[[-w / 2, -d / 2, w / 2, d / 2]])
    elif name == 'industrialcomplex':
        S['roof'] = dict(S['roof'], model='inset', form='hip', profile=[[0.0, S['roof']['eave']], [.6, h * .95], [1.0, h * .95]], inradius=min(w, d) / 2)
    elif name == 'greenhouse':
        S['features'] = []
    return S


@pytest.mark.parametrize('name', SHAPE_FIX)
def test_run1_gross_shapes_fail(name, monkeypatch):
    import shell
    real = shell.load
    old = _old_shape(name)
    monkeypatch.setattr(shell, 'load', lambda n: old if n == name else real(n))
    k = validate.build_game(name, 'pristine')
    fails, _ = validate.check_shape(name, k, validate.drafts.SIZES[name], params(name), S=real(name))
    kinds_ = {f.split(':')[0] for f in fails}
    assert kinds_ & {'footprint', 'roof', 'shape'}, fails
    if name in ('smallfueltank', '2civliving'):         # the raised floors also catch these on the silhouette
        sil = validate.silhouette(name, k, validate.drafts.SIZES[name], 6)
        floors, mean_floor = validate.arch_floors(params(name), validate.thresholds())
        assert any(r['iou'] < floors[v] for v, r in sil.items()) or sum(r['iou'] for r in sil.values()) / 5 < mean_floor


def test_structural_waivers_are_rejected():
    for chk, keys in (('silhouette', dict(view='top')), ('fit', dict(axis='z')), ('round', dict(part='x')), ('size', {}), ('archetype', {})):
        k, f = gate('2civliving', lambda p, c=chk, kk=keys: p['waivers'].append(dict(check=c, reason='stock top view shows something else entirely', **kk)))
        assert 'waiver' in k, (chk, f)
    assert not contract.waived(dict(waivers=[dict(check='silhouette', view='top', reason='x' * 30)]), 'silhouette', view='top')


def test_shape_capability_gap_is_needs_builder():
    import shell
    S = json.loads((ASSETS / 'smallfueltank.shape.json').read_text())
    assert shell.unsupported(S) is None
    S['roof'] = dict(S['roof'], model='extrude', axis='y', form='gable')
    assert 'roof' in shell.unsupported(S)
    S['footprint'] = dict(kind='irregular', n=0, edges=[])
    assert 'footprint' in shell.unsupported(S)


def test_analysis_edit_is_detected(tmp_path, monkeypatch):
    src = json.loads((ASSETS / 'tent.analysis.json').read_text())
    src['counts']['doors']['n'] = 0
    (tmp_path / 'tent.analysis.json').write_text(json.dumps(src))
    monkeypatch.setattr(analyze, 'ASSETS', tmp_path)
    assert contract.analysis_intact('tent')


def test_thresholds_edit_is_detected(tmp_path, monkeypatch):
    f = tmp_path / 'validate_thresholds.json'
    f.write_text(json.dumps(dict(json.loads(contract.THRESH.read_text()), iou_mean_min=.0)))
    monkeypatch.setattr(contract, 'THRESH', f)
    assert contract.thresholds_intact()


@pytest.mark.parametrize('name', PROOF)
def test_stamps_match_files(name):
    st = params(name).get('status', {}).get('validated')
    if st:
        assert st['params_sha'] == contract.params_sha(params(name))
        assert st['analysis_sha'] == archetypes.analysis_sha(name)


# ------------------------------------------------------------------ P0-2 counts from evidence
def test_invented_counts_fail():
    def edit(p):
        p['counts'].update(windows=6, doors=2)
    k, f = gate('civstoragea', edit)
    assert 'counts' in k, f


def test_omitted_counts_fail():
    def edit(p):
        p['counts'] = {k: 0 for k in p['counts']}
        p['features'] = []
    k, f = gate('barracks', edit)
    assert 'counts' in k, f


def test_count_waiver_needs_a_reason():
    def edit(p):
        p['counts']['windows'] = 6
        p['waivers'] = [dict(check='counts', field='windows', reason='x')]
    k, _ = gate('civstoragea', edit)
    assert 'waiver' in k and 'counts' in k


def test_count_waiver_with_reason_passes_the_count_gate():
    def edit(p):
        p['counts']['windows'] = 6
        p['waivers'] = p['waivers'] + [dict(check='counts', field='windows', reason='six windows are painted on the rear wall (side view)')]
    k, f = gate('civstoragea', edit)
    assert 'counts' not in k, f


# ------------------------------------------------------------------ P0-3 painted features, per side, both directions
def test_windows_moved_to_the_wrong_side_fail_both_directions():
    def edit(p):
        for f in p['features']:
            if f['kind'] == 'window':
                f['side'] = 'front'
    res = run('civstoragea', edit)
    msgs = [f for f in res['failures'] if f.startswith('features:')]
    assert any('INVENTS' in m for m in msgs), res['failures']      # model windows where the stock paints none
    assert any('lacks' in m for m in msgs), res['failures']        # painted stock windows the model lacks


def test_barracks_without_painted_openings_fails():
    def edit(p):
        p['features'] = []
        p['counts'].update(doors=0, windows=0)
    res = run('barracks', edit)
    assert 'features' in kinds(res) and 'counts' in kinds(res), res['failures']


# ------------------------------------------------------------------ P0-4 palette
def _grey(p):
    p['palette'] = {k: [.3, .3, .3] for k in archetypes.PAL_KEYS}


def _pink(p):
    p['palette']['main'] = [1, 0, .5]


def _empty(p):
    p['palette'] = {}


@pytest.mark.parametrize('edit', [_grey, _pink, _empty])
def test_bad_palettes_fail(edit):
    res = run('civstoragea', edit)            # the area-weighted mean dE needs the model (grey is within 18 of one stock dominant per slot)
    assert 'palette' in kinds(res) or 'schema' in kinds(res), res['failures']


def test_white_roof_slot_fails():
    k, f = gate('civstoragea', lambda p: p['palette'].update(roof=[.9, .9, .9], main=[.9, .9, .9]))
    assert 'palette' in k, f


# ------------------------------------------------------------------ P0-5 size and fit
def test_size_edit_fails():
    k, f = gate('barracks', lambda p: p.update(size=[x * .7 for x in p['size']]))
    assert 'size' in k, f


def test_fit_stretch_fails():
    def edit(p):
        p['dims']['eave_h'] = p['size'][2] * .15         # authored far too low: the fit would stretch z
    res = run('tent', edit)
    assert {'fit', 'silhouette', 'roof'} & set(kinds(res)), res['failures']


def test_gross_shape_dims_are_not_params():
    for key, val in (('roof', 'gable'), ('eave_h', 3.0), ('batter', {}), ('peaks', 3)):
        k, f = gate('barracks', lambda p, key=key, val=val: p['dims'].update({key: val}))
        assert 'schema' in k, (key, f)


# ------------------------------------------------------------------ P0-6 archetype lock and tagged fields
def test_archetype_retype_fails():
    k, f = gate('tent', lambda p: p.update(archetype='box'))
    assert 'archetype' in k, f


def test_turrets_are_manual():
    for n in ('srmturret', 'pulselaserturret', 'lrmturret'):
        assert archetypes.classify(dict(id=n, category='turret', identity=''))['archetype'] == 'manual'


def test_every_archetype_tags_required_fields():
    for a in archetypes.ARCHETYPES:
        assert arch_props.TAGGED.get(a), a


def test_unverifiable_field_fails():
    k, f = gate('woodenfence', lambda p: p['counts'].update(doors=1))
    assert 'counts' in k and any('not verifiable' in x for x in f), f


# ------------------------------------------------------------------ P0-7/P1-10 schema
def test_unknown_dims_keys_fail():
    k, f = gate('barracks', lambda p: p['dims'].update(roofs='gable', eave='9'))
    assert 'schema' in k, f


def test_unknown_params_key_fails():
    k, f = gate('barracks', lambda p: p.update(extra=1))
    assert 'schema' in k, f


def test_budget_cannot_be_raised():
    p = params('barracks')
    p['budget'] = 99999
    assert archetypes.budget_of(p) == archetypes.class_budget(p)
    k, _ = gate('barracks', lambda q: q.update(budget=99999))
    assert 'budget' in k


def test_evidence_edit_fails():
    def edit(p):
        p['evidence']['counts']['windows'] = 0
    k, f = gate('civstoragea', edit)
    assert 'evidence' in k, f


# ------------------------------------------------------------------ roof line (sawtooth) and LOD / round checks
def _cyl_soup(n, r=3.0, h=6.0):
    a = np.linspace(0, 2 * np.pi, n, endpoint=False)
    ring = np.stack([r * np.cos(a), r * np.sin(a)], 1)
    tris = []
    for i in range(n):
        p0, p1 = ring[i], ring[(i + 1) % n]
        b0, b1, t0, t1 = [*p0, 0], [*p1, 0], [*p0, h], [*p1, h]
        tris += [[b0, b1, t1], [b0, t1, t0]]
    return np.array(tris, float)


@pytest.mark.parametrize('n,ok', [(32, True), (8, False)])
def test_round_part_ring_check_lod0(n, ok):
    e = [dict(part='tank', kind='cylinder', R=3.0, center=[0, 0], z0=0, z1=6)]
    assert (validate.check_round(_cyl_soup(n), e, 0, {}) == []) is ok


def test_lod_ratio_check(tmp_path):
    pbr = tmp_path / 'pbr'
    pbr.mkdir()
    (pbr / 'x_pristine-bake.json').write_text(json.dumps(dict(lods=[dict(triangles=1000), dict(triangles=900), dict(triangles=700)], lights=[])))
    out = validate.check_baked('x', tmp_path, 3000, 0, validate.thresholds(), {}, None, None)
    assert any('LOD ratios' in f for f in out), out


# ------------------------------------------------------------------ P0-1 pick, P0-8 commit gate
def test_parked_assets_leave_the_batch(tmp_path, monkeypatch):
    monkeypatch.setattr(batch, 'STATUS', tmp_path / 'status.json')
    args = SimpleNamespace(assets=None, category='building', next=3, retry_failed=False)
    first = batch.pick(args)
    res = dict(failures=['roof: the model top surface follows the stock on 30% of the plan'])
    assert batch.park(first[0], res) == 'needs_builder'
    assert first[0] not in batch.pick(args)
    assert first[0] not in batch.pick(SimpleNamespace(assets=None, category=None, next=50, retry_failed=True))
    res = dict(failures=['palette: slot main dE 30'])
    assert [batch.park(first[1], res) for _ in range(batch.MAX_ROUNDS)][-1] == 'stuck'
    assert first[1] not in batch.pick(args)
    assert first[1] in batch.pick(SimpleNamespace(assets=None, category=None, next=50, retry_failed=True))   # --retry-failed, no category


def test_size_budgets():
    assert archetypes.class_budget(dict(archetype='box', size=[10, 10, 20])) == 15000          # grainstorage-class silo
    assert archetypes.class_budget(dict(archetype='container', size=[12, 8, 5])) == 8000       # building-sized prop
    assert archetypes.class_budget(dict(archetype='barrel', size=[1, 1, 1.2])) == 3000


def test_commit_refuses_red_kit_pytest(tmp_path, monkeypatch):
    monkeypatch.setattr(batch, 'STATUS', tmp_path / 'status.json')
    p = params('woodenfence')
    batch.set_status(['woodenfence'], 'passed', psha=contract.params_sha(p))
    monkeypatch.setattr(batch, 'stamp_ok', lambda *a: True)
    monkeypatch.setattr(validate, 'validate', lambda n, pass_dir=None: dict(asset=n, ok=True, failures=[], notes=[], waivers=[], metrics={}))
    monkeypatch.setattr(batch, 'content_ir', lambda tag, n: [])
    monkeypatch.setattr(batch, 'kit_pytest', lambda n: ['pytest: kit pytest -k woodenfence is red'])
    before = (ASSETS / 'woodenfence.params.json').read_text()
    with pytest.raises(SystemExit):
        batch.commit(SimpleNamespace(assets='woodenfence', tag='arch-proof', commit=True))
    assert (ASSETS / 'woodenfence.params.json').read_text() == before


def test_pick_reselects_failed_assets(tmp_path, monkeypatch):
    monkeypatch.setattr(batch, 'STATUS', tmp_path / 'status.json')
    args = SimpleNamespace(assets=None, category='building', next=3, retry_failed=False)
    first = batch.pick(args)
    assert first
    batch.set_status([first[0]], 'failed')
    again = batch.pick(args)
    assert again[0] == first[0]
    batch.set_status([first[0]], 'passed')
    assert first[0] not in batch.pick(args)
    batch.set_status([first[0]], 'failed')
    assert batch.pick(SimpleNamespace(assets=None, category='building', next=3, retry_failed=True)) == [first[0]]


def test_commit_refuses_failing_assets(tmp_path, monkeypatch):
    monkeypatch.setattr(batch, 'STATUS', tmp_path / 'status.json')
    batch.set_status(['barracks'], 'failed')
    before = (ASSETS / 'barracks.params.json').read_text()
    args = SimpleNamespace(assets='barracks', tag='arch-proof', commit=True)
    with pytest.raises(SystemExit):
        batch.commit(args)
    assert (ASSETS / 'barracks.params.json').read_text() == before


def test_pristine_only_mode():
    """--states pristine: only the pristine state is built/validated; stamps are kept apart from the full-state ones."""
    assert batch.states_of(SimpleNamespace(states=None)) is None
    assert batch.states_of(SimpleNamespace(states='pristine')) == ['pristine']
    with pytest.raises(SystemExit):
        batch.states_of(SimpleNamespace(states='worn'))
    assert batch.tier_key('B', ['pristine']) != batch.tier_key('B', None)
    res = validate.validate('smallfueltank', pre=True, states=['pristine'])
    assert 'tris' in res['metrics'] or res['metrics'] is not None
    assert set(res['metrics'].get('tris', {'pristine': 0})) == {'pristine'}
