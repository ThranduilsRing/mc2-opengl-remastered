"""Budget schema, noise safety and HTML comparisons; no game required."""
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('budget_check', ROOT / 'tools/perf/budget_check.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


@pytest.fixture
def budget():
    return json.loads(tool.BUDGET.read_text(encoding='utf-8'))


def profile(value=0.5, noise='quiet'):
    return {'runs': {'mc2_01': {'noise': {'status': noise, 'reasons': [], 'external_cpu_scope':'reserved_cores',
                                       'external_cpu_p95':4, 'whole_machine_external_cpu_p95':30},
                              'zones': {'Frame': {'count': 10, 'total_ms': value * 10, 'p95_ms': value},
                                        'DrawScreen': {'count': 10, 'total_ms': 0.5, 'p95_ms': 0.08}}}}}


def test_schema_and_validation_only_path(budget, monkeypatch):
    tool.validate_budget(budget)
    # Validation may not import a profiler, launch games or write reports.
    monkeypatch.setattr(tool, 'compare', lambda *a: pytest.fail('comparison during schema check'))
    assert tool.main(['--validate-schema']) == 0


@pytest.mark.parametrize('mutate', [
    lambda b: b.update(version=1),
    lambda b: b['missions'].pop('battle'),
    lambda b: b['metrics']['cpu_frame_ms'].update(goal=-1),
    lambda b: b['metrics']['cpu_frame_ms'].update(goal=True),
    lambda b: b['metrics']['cpu_frame_ms'].update(goal=float('nan')),
    lambda b: b['metrics']['cpu_frame_ms'].update(strict=1),
    lambda b: b['missions']['mc2_01']['baseline'].update(gpu_frame_ms='unknown'),
    lambda b: b['missions']['mc2_01'].update(baseline_status='unknown'),
    lambda b: b.update(extra=True),
])
def test_schema_rejects_invalid_budget(budget, mutate):
    mutate(budget)
    with pytest.raises(ValueError):
        tool.validate_budget(budget)


@pytest.mark.parametrize('value', [0.01, 100])
def test_noise_annotates_without_rejecting(budget, value):
    data = profile(value, 'NOISY')
    data['runs']['mc2_03'] = data['runs']['mc2_01']
    result = tool.compare(data, budget)['mc2_03']
    assert result['metrics']['cpu_frame_ms']['status'] == ('PASS' if value < 1 else 'FAIL')
    assert result['noise']['status'] == 'NOISY'


def test_missing_and_contradictory_noise_are_inconclusive(budget):
    data = profile()
    del data['runs']['mc2_01']['noise']
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'  # missing camera coverage
    data = profile()
    data['runs']['mc2_01']['noise']['external_cpu_p95'] = 26
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'
    data = profile()
    data['errors'] = {'mc2_01': 'capture failed'}
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'


def test_strict_goals_deltas_and_missing_metrics(budget):
    for mark in budget['missions']['mc2_01']['bookmarks'].values():
        mark['cpu_frame_ms'] = 'unmeasured'
    data = profile(1)
    data['runs']['mc2_01']['bookmarks'] = {name: profile(1)['runs']['mc2_01']
                                         for name in tool.CAMERA_NAMES}
    result = tool.compare(data, budget, profile(2))['mc2_01']
    frame = result['metrics']['cpu_frame_ms']
    assert frame['status'] == 'FAIL'  # owner says under 1 ms
    assert frame['delta_baseline_ms'] is None  # old single-view baseline is not an envelope baseline
    assert frame['distance_goal_ms'] == 0
    assert frame['delta_previous_ms'] == -1
    assert result['metrics']['gpu_frame_ms']['status'] == 'INCONCLUSIVE'
    assert result['baseline_status'] == 'NOISY'
    assert tool.compare(profile(), budget)['battle']['status'] == 'INCONCLUSIVE'


@pytest.mark.parametrize('invalid', [{'external_cpu_scope':None}, {'external_cpu_p95':None}, {'external_cpu_p95':15}])
def test_load_never_excludes_measurement(budget, invalid):
    run=profile()['runs']['mc2_01']
    data={'runs':{'mc2_03':run}}
    run['noise'].update(external_cpu_p95=14.99,whole_machine_external_cpu_p95=90)
    assert tool.compare(data,budget)['mc2_03']['metrics']['cpu_frame_ms']['status']=='PASS'
    run['noise'].update(invalid)
    assert tool.compare(data,budget)['mc2_03']['metrics']['cpu_frame_ms']['status']=='PASS'


def test_supplementary_metrics_require_context(budget):
    data = profile()
    run = data['runs']['mc2_01']
    run['measurements'] = {'gpu_frame_ms': {'value': 5, 'resolution': [3840, 2160]},
                           'mover_tick_ms': {'value': 0.6, 'movers': 200, 'ticks_per_sample': 1}}
    data['runs']['mc2_01'] = {'noise': run['noise'], 'bookmarks': {name: run.copy() for name in tool.CAMERA_NAMES}}
    assert tool.compare(data, budget)['mc2_01']['status'] == 'PASS'
    run['measurements']['gpu_frame_ms']['resolution'] = [1920, 1080]
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'
    run['measurements']['mover_tick_ms']['ticks_per_sample'] = 2
    assert 'mover_tick_ms' not in tool.measurements(run)


def test_html_and_cli_artifacts(budget, tmp_path):
    data = profile()
    data['runs']['mc2_01']['noise'].update(external_cpu_p95=4, whole_machine_external_cpu_p95=31)
    data['runs']['mc2_01']['zones']['<script>alert(1)</script>'] = {'count': 5, 'total_ms': 1, 'p95_ms': 0.5}
    source = tmp_path / 'profile.json'
    source.write_text(json.dumps(data), encoding='utf-8')
    output = tmp_path / 'report'
    assert tool.main(['--profile', str(source), '--previous', str(source), '--out', str(output)]) == 2
    html = (output / 'index.html').read_text(encoding='utf-8')
    assert 'viewport' in html and 'Δ previous' in html and 'NOISY' in html
    assert '<script>' not in html and '&lt;script&gt;' in html
    assert 'reserved cores +4.000%' in html and 'whole machine +31.000%' in html
    assert json.loads((output / 'budget.json').read_text())['mc2_01']['metrics']['cpu_frame_ms']['delta_previous_ms'] == 0


def test_cli_failure_and_invalid_budget(tmp_path):
    source = tmp_path / 'bad.json'
    source.write_text('{}')
    assert tool.main(['--budget', str(source), '--validate-schema']) == 3


def test_worst_bookmark_and_missing_coverage(budget):
    run = profile()['runs']['mc2_01']
    run['bookmarks'] = {name: profile(0.1)['runs']['mc2_01'] for name in tool.CAMERA_NAMES}
    run['bookmarks']['orbit'] = profile(9)['runs']['mc2_01']
    data = {'runs': {'mc2_01': run}}
    result = tool.compare(data, budget)['mc2_01']['metrics']['cpu_frame_ms']
    assert (result['value'], result['worst_bookmark'], result['status']) == (9, 'orbit', 'FAIL')
    for mark in budget['missions']['mc2_01']['bookmarks'].values():
        mark['cpu_frame_ms'] = 1
    budget['missions']['mc2_01']['bookmarks']['wolfman']['cpu_frame_ms'] = 5
    assert tool.compare(data, budget)['mc2_01']['metrics']['cpu_frame_ms']['delta_baseline_ms'] == 4
    del run['bookmarks']['wolfman']
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'


def test_unmeasured_bookmark_metrics_do_not_pass(budget):
    budget['missions']['mc2_01']['bookmarks']['orbit']['cpu_frame_ms'] = 'unknown'
    with pytest.raises(ValueError, match='values'):
        tool.validate_budget(budget)


def test_capture_mode_preserves_profile_and_errors(tmp_path, monkeypatch):
    install = tmp_path / 'install'
    install.mkdir()
    (install / 'mc2.exe').write_bytes(b'test exe')
    seen = []

    def capture(args, name, directory, psutil):
        seen.append((name, args.seconds, args.install))
        if name == 'battle':
            raise RuntimeError('fixture capture error')
        return profile()['runs']['mc2_01']

    monkeypatch.setitem(sys.modules, 'mission_cpu_profile', SimpleNamespace(profile=capture))
    priorities = []
    monkeypatch.setitem(sys.modules, 'psutil', SimpleNamespace(
        Process=lambda: SimpleNamespace(nice=priorities.append), IDLE_PRIORITY_CLASS=64))
    output = tmp_path / 'proof'
    assert tool.main(['--install', str(install), '--out', str(output)]) == 2
    assert {name for name, _, _ in seen} == tool.MISSIONS
    assert all(seconds == 20 and path == install.resolve() for _, seconds, path in seen)
    if sys.platform == 'win32':
        assert priorities == [64]
    saved = json.loads(next(output.glob('*/profile.json')).read_text())
    assert saved['errors'] == {'battle': 'fixture capture error'}
    assert len(saved['exe_sha256']) == 64
    assert next(output.glob('*/index.html')).is_file()
