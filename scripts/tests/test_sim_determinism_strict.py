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


def test_only_nonsim_rng_counts_are_excluded(tmp_path):
    a = observations()
    b = [line.replace("rng=1/2/3", "rng=1/5/6") for line in a]   # appearance and weather draws differ, sim draws agree
    assert sim.chain(a) == sim.chain(b)
    assert sim.full_chain(a) != sim.full_chain(b)
    ok, message = sim.compare(a, b, 5)
    assert ok and "1/2/3" in message and "1/5/6" in message
    assert sim.check_baseline(b, baseline(tmp_path, a))[0]
    assert not sim.check_baseline(b, baseline(tmp_path, a, legacy=True))[0]


def test_extra_sim_rng_draw_fails(tmp_path):
    a = observations()
    b = [line.replace("rng=1/2/3", "rng=2/2/3") for line in a]
    assert sim.chain(a) != sim.chain(b)
    assert not sim.compare(a, b, 5)[0]
    assert not sim.check_baseline(b, baseline(tmp_path, a))[0]
    assert not sim.compare(a, [line.replace(" rng=1/2/3", "") for line in a], 5)[0]   # a missing field is an incomplete observation


def test_rejected_log_leaves_existing_baseline(tmp_path, monkeypatch):
    log = tmp_path / "bad.log"
    good = observations()
    log.write_text("\n".join(good[:2] + good[3:]) + "\n", encoding="utf-8")   # a skipped tick
    out = tmp_path / "base.json"
    out.write_text("KEEP", encoding="utf-8")
    monkeypatch.setattr(sim, "data_fingerprint", lambda *a, **k: {"digest": "d", "problems": []})
    monkeypatch.setattr(sys, "argv", ["sim_determinism.py", "write-baseline", str(log), "--frames", "4", "--mission", "m", "--out", str(out)])
    with pytest.raises(SystemExit):
        sim.main()
    assert out.read_text(encoding="utf-8") == "KEEP"
    assert not (tmp_path / "base.json.tmp").exists()


@pytest.mark.parametrize("argv,expected", [(["run", "mc2_03"], "20"), (["check", "mc2_03"], "20"), (["check", "mc2_03", "--duration", "40"], "40")])
def test_determinism_runs_default_to_20s(monkeypatch, argv, expected):
    seen = []
    def fake(mission, duration, *a, **kw):
        seen.append(duration)
        raise RuntimeError("stop")
    monkeypatch.setattr(sim, "run_mission", fake)
    with pytest.raises(RuntimeError):
        sim.main(argv)
    assert seen == [expected]


@pytest.mark.parametrize("mission", ["mc2_03", "mc2_10", "mc2_16", "mc2_24"])
def test_committed_baseline_is_a_schema2_chain(mission):
    """The offline half of the baseline check: a legacy full-line baseline (no chain_schema) hashes animrng/appearance counts and
    breaks on every format change; the live half is `tools/sim_determinism.py check <mission>` at merge gates."""
    base = json.loads((Path(sim.BASELINE_DIR) / f"{mission}.json").read_text(encoding="utf-8"))
    assert base.get("chain_schema") == 2
    assert base["frames"] >= sim.MIN_FRAMES and len(base["chain"]) == 16
    assert base["data_fingerprint"]["digest"] and base["seed"]
