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
    return {'runs': {'mc2_01': {'noise': {'status': noise, 'reasons': []},
                              'zones': {'Frame': {'count': 10, 'total_ms': value * 10, 'p95_ms': value},
                                        'DrawScreen': {'count': 10, 'total_ms': 0.5, 'p95_ms': 0.08}}}}}


def test_schema_and_validation_only_path(budget, monkeypatch):
    tool.validate_budget(budget)
    # Validation may not import a profiler, launch games or write reports.
    monkeypatch.setattr(tool, 'compare', lambda *a: pytest.fail('comparison during schema check'))
    assert tool.main(['--validate-schema']) == 0


@pytest.mark.parametrize('mutate', [
    lambda b: b.update(version=2),
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
def test_noise_never_passes_or_fails(budget, value):
    result = tool.compare(profile(value, 'NOISY'), budget)['mc2_01']
    assert result['status'] == 'INCONCLUSIVE'
    assert {r['status'] for r in result['metrics'].values()} == {'INCONCLUSIVE'}


def test_missing_and_contradictory_noise_are_inconclusive(budget):
    data = profile()
    del data['runs']['mc2_01']['noise']
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'
    data = profile()
    data['runs']['mc2_01']['noise']['external_cpu_p95'] = 26
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'
    data = profile()
    data['errors'] = {'mc2_01': 'capture failed'}
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'


def test_strict_goals_deltas_and_missing_metrics(budget):
    result = tool.compare(profile(1), budget, profile(2))['mc2_01']
    frame = result['metrics']['cpu_frame_ms']
    assert frame['status'] == 'FAIL'  # owner says under 1 ms
    assert frame['delta_baseline_ms'] == pytest.approx(-0.644)
    assert frame['distance_goal_ms'] == 0
    assert frame['delta_previous_ms'] == -1
    assert result['metrics']['gpu_frame_ms']['status'] == 'INCONCLUSIVE'
    assert result['baseline_status'] == 'NOISY'
    assert tool.compare(profile(), budget)['battle']['status'] == 'INCONCLUSIVE'


def test_supplementary_metrics_require_context(budget):
    data = profile()
    run = data['runs']['mc2_01']
    run['measurements'] = {'gpu_frame_ms': {'value': 5, 'resolution': [3840, 2160]},
                           'mover_tick_ms': {'value': 0.6, 'movers': 200, 'ticks_per_sample': 1}}
    assert tool.compare(data, budget)['mc2_01']['status'] == 'PASS'
    run['measurements']['gpu_frame_ms']['resolution'] = [1920, 1080]
    assert tool.compare(data, budget)['mc2_01']['status'] == 'INCONCLUSIVE'
    run['measurements']['mover_tick_ms']['ticks_per_sample'] = 2
    assert 'mover_tick_ms' not in tool.measurements(run)


def test_html_and_cli_artifacts(budget, tmp_path):
    data = profile()
    data['runs']['mc2_01']['zones']['<script>alert(1)</script>'] = {'count': 5, 'total_ms': 1, 'p95_ms': 0.5}
    source = tmp_path / 'profile.json'
    source.write_text(json.dumps(data), encoding='utf-8')
    output = tmp_path / 'report'
    assert tool.main(['--profile', str(source), '--previous', str(source), '--out', str(output)]) == 2
    html = (output / 'index.html').read_text(encoding='utf-8')
    assert 'viewport' in html and 'Δ previous' in html and 'NOISY' in html
    assert '<script>' not in html and '&lt;script&gt;' in html
    assert json.loads((output / 'budget.json').read_text())['mc2_01']['metrics']['cpu_frame_ms']['delta_previous_ms'] == 0


def test_cli_failure_and_invalid_budget(tmp_path):
    source = tmp_path / 'bad.json'
    source.write_text('{}')
    assert tool.main(['--budget', str(source), '--validate-schema']) == 3


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
