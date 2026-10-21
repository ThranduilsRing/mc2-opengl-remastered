"""GPU evidence rejects incomparable captures and keeps window means honest."""
from copy import deepcopy
import importlib.util
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location('terrain_perf_report', Path(__file__).resolve().parents[2] / 'tools/terrain_perf_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


@pytest.fixture
def evidence():
    return {'identity': dict(commit='a' * 40, exe_sha256='b' * 64, shaders_sha256='c' * 64,
                             content_sha256='d' * 64, config='RelWithDebInfo', gpu='7900 XTX',
                             driver='test', resolution=[1920, 1080], settings={'POM': True},
                             mission='mc2_01', camera=[1, 2, 3, 4, 5]),
            'sampling': 'frame', 'scopes': {report.TERRAIN: [1, 2, 3, 4], 'VKIsl.Water': [0.1]}}


@pytest.mark.parametrize('key', report.IDENTITY)
def test_missing_identity_refused(evidence, key):
    del evidence['identity'][key]
    with pytest.raises(ValueError, match='identity'):
        report.summarize(evidence)


@pytest.mark.parametrize('key,value', [('resolution', [3840, 2160]), ('commit', 'e' * 40),
                                     ('exe_sha256', 'f' * 64), ('settings', {'POM': False}),
                                     ('camera', [9]), ('driver', 'other'), ('content_sha256', 'e' * 64)])
def test_mismatch_refused(evidence, key, value):
    after = deepcopy(evidence)
    after['identity'][key] = value
    with pytest.raises(ValueError, match=key):
        report.compare(evidence, after)


def test_explicit_candidate_build(evidence):
    after = deepcopy(evidence)
    after['identity'].update(commit='e' * 40, exe_sha256='f' * 64, shaders_sha256='0' * 64)
    after['scopes'][report.TERRAIN] = [1]
    delta = report.compare(evidence, after, 'e' * 40)['delta']
    assert delta[report.TERRAIN] == {'before_ms': 2.5, 'after_ms': 1, 'delta_ms': -1.5}
    with pytest.raises(ValueError, match='candidate'):
        report.compare(evidence, after, '0' * 40)


@pytest.mark.parametrize('samples', [[], [float('nan')], [-1], [float('inf')], [True]])
def test_bad_or_missing_timings(evidence, samples):
    evidence['scopes'][report.TERRAIN] = samples
    with pytest.raises(ValueError):
        report.summarize(evidence)


def test_separate_scopes_and_real_percentiles(evidence):
    result = report.summarize(evidence)
    assert result['scopes'][report.TERRAIN]['mean_ms'] == 2.5
    assert result['scopes'][report.TERRAIN]['p95_ms'] == 4
    assert result['scopes']['VKIsl.Water']['mean_ms'] == 0.1
    assert report.compare(evidence, evidence)['delta'][report.TERRAIN]['delta_ms'] == 0


def log_row(frame=720, n=120, dropped=0, overflow=0):
    return (f'[VK_GPU_PASSES v1] frame={frame} n={n} span_ms=5.000 sum_ms=4.900 '
            f'unattributed_ms=0.100 scopes=2 dropped={dropped} overflow={overflow} | '
            'VKIsl.Terrain=4.530(x1.0) VKIsl.Water=0.123(x1.0)')


def test_log_warmup_and_window_means(evidence):
    imported = report.from_log(log_row(600) + '\n' + log_row(), evidence['identity'], 600)
    result = report.summarize(imported)
    assert imported['windows'] == [{'end_frame': 720, 'frames': 120}]
    assert result['scopes'][report.TERRAIN] == {'count': 1, 'mean_ms': 4.53}
    assert len(imported['source_log_sha256']) == 64


@pytest.mark.parametrize('log', ['', log_row(dropped=1), log_row(overflow=1), log_row(n=0),
                               log_row() + '\n' + log_row(840, n=60),
                               log_row().replace('VKIsl.Terrain=', 'VKIsl.Other=')])
def test_incomplete_logs_refused(evidence, log):
    with pytest.raises(ValueError):
        report.from_log(log, evidence['identity'], 600)


def test_scope_or_sampling_mismatch(evidence):
    after = deepcopy(evidence)
    after['sampling'] = 'window_mean'
    with pytest.raises(ValueError, match='scopes differ'):
        report.compare(evidence, after)
    after['sampling'] = 'frame'
    del after['scopes']['VKIsl.Water']
    with pytest.raises(ValueError, match='scopes differ'):
        report.compare(evidence, after)
