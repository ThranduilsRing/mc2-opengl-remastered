"""Seed changes at the actual consumed-feed and activity comparison boundary."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location('terrain_feed_compare', Path(__file__).resolve().parents[2] / 'tools/terrain_feed_compare.py')
feed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feed)


@pytest.fixture
def receipt():
    return dict(island='terrain', schema=1, render_frame=60, activity_frame=60, frozen=True,
                resolution=[1920, 1080], mapSide=2, patchCount=1,
                patches=[[0, 0, 1, 1, 1, 0, 15, 0, 0]], ubo_hex='00' * 1024,
                fields={k: {'bytes': 16, 'fnv1a64': 10} for k in feed.FIELDS},
                versions=[1] * 9, imported_versions=[1] * 8, resource_shapes=[1] * 11,
                material_handles=[1] * 9, external_image_contents='not_read_back',
                activity=dict(blocks='01', vertices='01010100', solid_enabled=True,
                              solid_count=2, solid_hex='0100000001000000'))


def test_identical(receipt):
    assert feed.compare(receipt, deepcopy(receipt)) == []


@pytest.mark.parametrize('name', sorted(feed.FIELDS))
def test_seeded_content_change(receipt, name):
    after = deepcopy(receipt)
    after['fields'][name]['fnv1a64'] += 1
    assert feed.compare(receipt, after) == ['fields']


@pytest.mark.parametrize('key', ['versions', 'imported_versions', 'resource_shapes', 'material_handles'])
def test_seeded_version_or_binding_change(receipt, key):
    after = deepcopy(receipt)
    after[key][-1] += 1
    assert feed.compare(receipt, after) == [key]


@pytest.mark.parametrize('index', range(9))
def test_each_patch_field_is_observed(receipt, index):
    after = deepcopy(receipt)
    after['patches'][0][index] += 1
    assert feed.compare(receipt, after) == ['patches']


def test_one_uniform_bit(receipt):
    after = deepcopy(receipt)
    after['ubo_hex'] = '01' + after['ubo_hex'][2:]
    assert feed.compare(receipt, after) == ['ubo_hex']


@pytest.mark.parametrize('key', ['blocks', 'vertices', 'solid_hex'])
def test_protected_activity(receipt, key):
    after = deepcopy(receipt)
    after['activity'][key] = '00' + after['activity'][key][2:]
    assert feed.compare(receipt, after) == ['activity']


def test_frame_epoch_change(receipt):
    after = deepcopy(receipt)
    after['render_frame'] += 1
    after['activity_frame'] += 1
    assert feed.compare(receipt, after) == ['activity_frame', 'render_frame']


def test_stale_activity_refused(receipt):
    receipt['activity_frame'] -= 1
    with pytest.raises(ValueError, match='epoch'):
        feed.validate(receipt)


def test_stale_consumed_height_refused(receipt):
    receipt['imported_versions'][0] += 1
    with pytest.raises(ValueError, match='stale consumed height'):
        feed.validate(receipt)


@pytest.mark.parametrize('value', [None, -1, 2**64])
def test_missing_or_invalid_height_refused(receipt, value):
    receipt['fields']['height']['fnv1a64'] = value
    with pytest.raises(ValueError):
        feed.validate(receipt)


@pytest.mark.parametrize('key', sorted(feed.REQUIRED))
def test_missing_sections_refused(receipt, key):
    del receipt[key]
    with pytest.raises(ValueError):
        feed.validate(receipt)


def test_unfrozen_refused(receipt):
    receipt['frozen'] = False
    with pytest.raises(ValueError, match='frozen'):
        feed.validate(receipt)


def test_duplicate_receipts_refused(receipt, tmp_path):
    path = tmp_path / 'feed.jsonl'
    path.write_text((json.dumps(receipt) + '\n') * 2)
    with pytest.raises(ValueError, match='exactly one'):
        feed.load(path)


def test_non_object_refused(tmp_path):
    path = tmp_path / 'feed.jsonl'
    path.write_text('[]\n')
    with pytest.raises(ValueError, match='JSON objects'):
        feed.load(path)


def test_cli_exit_codes(receipt, tmp_path):
    a, b = tmp_path / 'a', tmp_path / 'b'
    a.write_text(json.dumps(receipt))
    b.write_text(json.dumps(receipt))
    assert feed.main([str(a), str(b)]) == 0
    receipt['activity']['blocks'] = '00'
    b.write_text(json.dumps(receipt))
    assert feed.main([str(a), str(b)]) == 1
    b.write_text('{}')
    with pytest.raises(SystemExit) as exc:
        feed.main([str(a), str(b)])
    assert exc.value.code == 2
