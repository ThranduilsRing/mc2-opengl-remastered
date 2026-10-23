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

PROOF = ('barracks', 'civstoragea', 'dumpster', 'munitionscrates', 'tent', 'woodenfence')
ASSETS = ART / 'assets'


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
        p['dims']['eave_h'] = p['size'][2] * .5          # authored far too low: the fit stretches z by ~2
    res = run('barracks', edit)
    assert 'fit' in kinds(res), res['failures']


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
def test_sawtooth_roof_needs_its_peaks():
    def edit(p):
        p['dims'].update(roof='flat')
        p['dims'].pop('peaks', None)
        p['dims'].pop('rise', None)
        p['dims'].pop('eave_h', None)
    res = run('civstoragea', edit)
    assert any('ridge' in f for f in res['failures']) or 'silhouette' in kinds(res), res['failures']


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
