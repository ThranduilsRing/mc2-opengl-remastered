"""Stock Pine1/2/3 render as the cooked Poly Haven conifers on the island-tree LOD chain (owner 11-14, 11-23)."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REG = json.loads((ROOT / 'data/model_overrides/models.json').read_text(encoding='utf-8'))['overrides']
PINS = json.loads((ROOT / 'intentional-asset-changes.json').read_text(encoding='utf-8'))
BY_KEY = {r['replaces']: r for r in REG}


def _chain(rec):
    return [rec['source']] + [l['source'] for l in rec['lods']]


def test_every_live_stock_pine_is_replaced_and_dead_pines_stay_stock():
    for n in (1, 2, 3):
        rec = BY_KEY[f'tree:pine{n}']
        assert rec['source'] == f'conifer_pine{n}/pristine_intact_lod0.glb' and rec['scale'] == 1.0
        assert f'tree:pine{n}dead' not in BY_KEY


def test_conifers_use_the_island_lod_levels_with_rising_distances():
    island = [l['lod'] for l in BY_KEY['tree:tc1_1']['lods']]
    for n in (1, 2, 3):
        lods = BY_KEY[f'tree:pine{n}']['lods']
        assert [l['lod'] for l in lods] == island == [1, 2, 3, 4]
        dist = [l['distance'] for l in lods]
        assert dist == sorted(dist) and len(set(dist)) == 4 and dist[0] > 0
        assert [Path(p).stem.rsplit('_', 1)[1] for p in _chain(BY_KEY[f'tree:pine{n}'])] == ['lod0', 'lod1', 'lod2', 'lod3', 'far']


def test_varied_heights_switch_lods_at_height_scaled_distances():
    first = [BY_KEY[f'tree:pine{n}']['lods'][0]['distance'] for n in (1, 2, 3)]
    assert len(set(first)) == 3


def test_every_conifer_file_is_pinned_for_deploy():
    for n in (1, 2, 3):
        rec = BY_KEY[f'tree:pine{n}']
        paths = _chain(rec) + [s[k] for s in rec['materials']['slots'] for k in ('normal', 'orm')]
        for p in paths:
            assert len(PINS.get(f'data/model_overrides/{p}', '')) == 64, p
