"""Negative controls for W1: raw bits and complete observations are mandatory."""
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import sim_determinism as sim


def observations():
    return [f"{sim.PREFIX}{i} t=0.000 n=2 graded=aa visual=bb strict=cc "
            "nobj=3 objects=dd nabl=1 abl=ee rng=1/2/3" for i in range(5)]


def baseline(tmp_path, lines, legacy=False):
    path = tmp_path / "baseline.json"
    data = {"frames": len(lines), "chain": sim.full_chain(lines) if legacy else sim.chain(lines)}
    if not legacy:
        data.update(chain_schema=2, full=sim.full_chain(lines))
    path.write_text(json.dumps(data))
    return str(path)


@pytest.mark.parametrize("legacy", [False, True])
def test_strict_only_divergence_fails(tmp_path, legacy):
    a = observations()
    b = a.copy()
    b[2] = b[2].replace("strict=cc", "strict=ff")
    assert not sim.compare(a, b, 5)[0]
    assert not sim.check_baseline(b, baseline(tmp_path, a, legacy))[0]
    assert sim.check_baseline(a, baseline(tmp_path, a, legacy))[0]


@pytest.mark.parametrize("count", ["n=2", "nobj=3", "nabl=1"])
def test_missing_count_even_in_both_runs_fails(tmp_path, count):
    lines = [line.replace(" " + count, "") for line in observations()]
    assert not sim.compare(lines, lines, 5)[0]
    assert not sim.check_baseline(lines, baseline(tmp_path, lines))[0]


def test_missing_tick_and_truncated_run_fail(tmp_path):
    a = observations()
    b = a[:2] + a[3:]
    assert not sim.compare(b, b, 3)[0]
    assert not sim.compare(a[:2], a[:2], 5)[0]
    assert not sim.check_baseline(a[:2], baseline(tmp_path, a))[0]


def test_rng_counts_reported_but_excluded(tmp_path):
    a = observations()
    b = [line.replace("rng=1/2/3", "rng=4/5/6") for line in a]
    assert sim.chain(a) == sim.chain(b)
    assert sim.full_chain(a) != sim.full_chain(b)
    ok, message = sim.compare(a, b, 5)
    assert ok and "1/2/3" in message and "4/5/6" in message
    assert sim.check_baseline(b, baseline(tmp_path, a))[0]
    assert not sim.check_baseline(b, baseline(tmp_path, a, legacy=True))[0]
