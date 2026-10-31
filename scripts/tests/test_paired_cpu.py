"""Robust paired effects and interleaving, without game processes."""
import importlib.util
from pathlib import Path
import random
from statistics import median
from types import SimpleNamespace

import pytest

PATH = Path(__file__).resolve().parents[2] / 'tools/perf/paired_cpu.py'
spec = importlib.util.spec_from_file_location('paired_cpu', PATH)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def test_known_effect_survives_hitches_and_drift():
    rng = random.Random(42)
    a, b = [], []
    for i in range(15):
        drift = i * .2
        for samples, effect in ((a, 0), (b, -.1)):
            events = [2 + drift + effect + rng.uniform(-.01, .01) +
                      (rng.uniform(2, 50) if rng.random() < .2 else 0) for _ in range(1000)]
            samples.append(median(events))
    result = tool.paired_delta(a, b)
    assert result['verdict'] == 'WIN'
    assert result['delta_ms'] == pytest.approx(-.1, abs=.005)
    assert result['ci90_ms'][1] < 0
    assert tool.paired_delta(b, a)['verdict'] == 'REGRESSION'


def test_null_and_uncertainty():
    assert tool.paired_delta([1] * 5, [1] * 5)['verdict'] == 'NO CHANGE'
    result = tool.paired_delta([1] * 15, [1 + x for x in [-.01, 0, .01] * 5])
    assert result['ci90_ms'][0] <= 0 <= result['ci90_ms'][1]
    assert result['verdict'] == 'NO CHANGE'
    assert tool.paired_delta([1] * 5, [0, 2, 1, 0, 2])['verdict'] == 'INCONCLUSIVE'
    assert tool.paired_delta([1], [.9])['verdict'] == 'INCONCLUSIVE'


@pytest.mark.parametrize('a,b', [([], []), ([1], [1, 2]), ([float('nan')], [1])])
def test_invalid_pairs(a, b):
    with pytest.raises(ValueError):
        tool.paired_delta(a, b)


def test_runner_interleaves_and_keeps_noisy_pairs(tmp_path, monkeypatch):
    installs = []
    for arm in ('a', 'b'):
        install = tmp_path / arm
        install.mkdir()
        (install / 'mc2.exe').write_bytes(arm.encode())
        installs.append(install)
    seen = []
    def capture(args, mission, directory, psutil):
        seen.append((args.install.name, mission, args.seconds, args.short_view))
        value = 1 if args.install.name == 'a' else .9
        return {'zones': {name: {'count': 10, 'total_ms': value * 10,
                'median_ms': value, 'p95_ms': value + .5, 'min_ms': value - .01, 'q25_ms': value}
                for name in tool.ZONES}, 'noise': {'status': 'NOISY', 'external_cpu_p95': 35}}
    monkeypatch.setattr(tool.cpu, 'profile', capture)
    args = SimpleNamespace(exe_a=None, exe_b=None, install_a=installs[0], install_b=installs[1],
                           out=tmp_path / 'proof', missions=['mc2_01'], pairs=5, seconds=20)
    assert tool.run(args, None) == 0
    assert [s[0] for s in seen] == ['a', 'b'] * 5
    import json
    report = json.loads((args.out / 'profile.json').read_text())
    row = report['comparisons']['mc2_01']['Frame']
    assert row['pairs'] == 5 and row['verdict'] == 'WIN'
    assert row['a']['reserved_external_cpu_p95'] == [35] * 5


def test_failures_are_preserved_not_turned_into_timings():
    assert tool.comparison([{'a': {'error': 'crash'}, 'b': {'zones': {}}}]) == {}
