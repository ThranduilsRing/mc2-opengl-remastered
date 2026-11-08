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
            "nobj=3 objects=dd nabl=1 abl=ee rng=1/2/3 animrng=4" for i in range(5)]


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


def test_merge_gate_refuses_red_smoke_even_with_a_sim_stream(tmp_path, monkeypatch):
    from types import SimpleNamespace
    source = tmp_path / "artifacts"
    source.mkdir()
    (source / "mc2_03.log").write_text("\n".join(observations()))
    monkeypatch.delenv("MC2_IN_GATE", raising=False)   # standalone runs are held to the same rule as the gate
    monkeypatch.setattr(sim.subprocess, "run", lambda *a, **k: SimpleNamespace(
        returncode=1, stdout=f"[runner] artifacts: {source}\nFAILED vk_validation_error\n"))
    with pytest.raises(SystemExit, match="smoke run a failed"):
        sim.run_mission("mc2_03", "20", str(tmp_path / "out"))
    assert not (tmp_path / "out/a").exists()


def run_check(monkeypatch, tmp_path, frames_by_duration, needed=1000):
    base = tmp_path / "base.json"
    base.write_text(json.dumps({"frames": needed}))
    monkeypatch.setattr(sim, "baseline_path", lambda m: str(base))
    monkeypatch.setattr(sim, "_load_env", lambda p: {})
    seen = []

    def fake_run(mission, duration, out, *a, **k):
        seen.append(duration)
        return f"a{duration}", f"b{duration}"
    monkeypatch.setattr(sim, "run_mission", fake_run)
    monkeypatch.setattr(sim, "load_simid", lambda p: [""] * frames_by_duration(int(p[1:])))
    monkeypatch.setattr(sim, "compare", lambda *a, **k: (True, "same"))
    monkeypatch.setattr(sim, "check_baseline", lambda *a, **k: (True, "ok"))
    monkeypatch.setattr(sim, "_seed", lambda p: "")
    monkeypatch.setattr(sim, "data_fingerprint", lambda i: {})
    rc = sim.main(["check", "mc2_x", "--out", str(tmp_path / "o")])
    return rc, seen


def test_check_preserves_requested_duration_on_loaded_host(monkeypatch, tmp_path):
    rc, seen = run_check(monkeypatch, tmp_path, lambda d: d * 42)  # 20 s -> 840, 29 s -> 1218
    assert rc == 3 and seen == ["20"]


def test_check_inconclusive_without_retry(monkeypatch, tmp_path, capsys):
    rc, seen = run_check(monkeypatch, tmp_path, lambda d: d * 5)
    assert rc == 3 and "INCONCLUSIVE (too few frames)" in capsys.readouterr().out
    assert seen == ["20"]


@pytest.mark.parametrize("frames", [1000, 2404, 2599, 2600])
def test_windowed_ticks_require_full_requested_run(monkeypatch, tmp_path, frames):
    fixture = tmp_path / "truncated.log"
    fixture.write_text("\n".join(observations() * (frames // 5) + observations()[:frames % 5]))
    monkeypatch.setattr(sim, "run_mission", lambda *a, **k: (str(fixture), str(fixture)))
    monkeypatch.setattr(sim, "compare", lambda *a, **k: (True, "same"))
    out = tmp_path / "out"
    rc = sim.main(["run", "mc2_24", "--ticks", "2600", "--out", str(out)])
    assert rc == (0 if frames == 2600 else 3)
    result = json.loads((out / "result.json").read_text())
    assert result["requested_ticks"] == result["required_frames"] == 2600
    assert result["frames_a"] == frames and result["complete"] == (frames == 2600)


def test_check_passes_when_requested_window_fills_baseline(monkeypatch, tmp_path):
    rc, seen = run_check(monkeypatch, tmp_path, lambda d: d * 60)
    assert rc == 0 and seen == ["20"]


_TICKS = "[SMOKE v1] event=tick_budget ticks=800\n[SMOKE v1] event=summary result=pass\n"
_RENDER_ROW = "| mc2_03  | FAIL   | heartbeat_freeze_play,perf_counters_missing,vegetation_floor_missing | 0 | - | - | - | - |\n"


@pytest.mark.parametrize("rc,text,ok", [
    (0, _TICKS, True),
    (1, _TICKS + _RENDER_ROW, True),                                                  # render-only buckets: headless by design
    (1, _TICKS + _RENDER_ROW + "[runner] [DEPLOY_FINGERPRINT] HARD FAIL: smoke ran an exe whose identity failed\n", False),
    (1, _TICKS + "> **DEPLOY_FINGERPRINT:** FAIL (required)\n" + _RENDER_ROW, False),
    (1, _TICKS + "[DEPLOY_FINGERPRINT] MISMATCH: exe sha=abc expected=def\n" + _RENDER_ROW, False),
    (1, _TICKS + "| mc2_03  | FAIL   | content_error,heartbeat_freeze_play | 0 | - |\n", False),  # a content failure stays red
    (1, _TICKS + "| mc2_03  | FAIL   | crash_no_summary | 0 | - |\n", False),
    (1, _TICKS, False),                                                               # rc=1 with no failed row: another gate failed
    (1, _TICKS.replace("result=pass", "result=fail") + _RENDER_ROW, False),           # failed engine verdict
    (1, _TICKS + "> **RUN INVALID — the missions' results do not count.**\n" + _RENDER_ROW, False),
    # headless has no Vulkan device: the runner's GPU-routing validity reason is by design, any other reason stays red
    (1, _TICKS + "> **RUN INVALID — the missions' results do not count.**\n>\n> - GPU: mc2_03: GPU routing: the run asked for a device matching 'NVIDIA' but its log has no [VK_PROFILE] device line\n" + _RENDER_ROW, True),
    (1, _TICKS + "> **RUN INVALID — the missions' results do not count.**\n>\n> - GPU: mc2_03: GPU routing: the run asked for a device matching 'NVIDIA' but its log has no [VK_PROFILE] device line\n> - host: another mc2.exe was running\n" + _RENDER_ROW, False),
    # headless has no play phase for the order-liveness fixture: its "summary line absent" verdict is by design, other liveness failures stay red
    (1, _TICKS + _RENDER_ROW + "[runner] [ORDER-LIVENESS] HARD FAIL: 1 order-liveness check(s) failed\n[runner]   ORDER-LIVENESS FAIL mc2_16: ORDER_LIVENESS_SUMMARY line absent from log (fixture never reached its summary phase)\n", True),
    (1, _TICKS + _RENDER_ROW + "[runner] [ORDER-LIVENESS] HARD FAIL: 1 order-liveness check(s) failed\n[runner]   ORDER-LIVENESS FAIL mc2_16: unit 3 never moved\n", False),
    (1, "[SMOKE v1] event=summary result=pass\n" + _RENDER_ROW, False),               # never reached the tick budget
    (0, _TICKS + "Crash Report\n", False),
    (2, _TICKS + _RENDER_ROW, False),
])
def test_headless_exit_ok_keeps_failed_verdicts_red(rc, text, ok):
    """W116 item 3: rc=1 is only the render-gate verdict of a headless run, never a fingerprint or content failure."""
    assert sim.headless_exit_ok(rc, text) is ok


def test_load_simid_drops_a_console_line_glued_to_animrng(tmp_path):
    """W125: a stderr [VK_VALIDATION v1] chunk landed between the SIMID's last digit and its newline ('animrng=27smatc')."""
    log = tmp_path / "x.log"
    log.write_text("[VK_VALIDATION v1] severity=warning id=Format-Mi[SIMID v1] simframe=126 rng=1/2/3 animrng=27smatc\n"
                   "h-ImageView text=\"x\"\n[SIMID v1] simframe=127 rng=1/2/3 animrng=28\n", encoding="utf-8")
    got = sim.load_simid(str(log))
    assert got == ["[SIMID v1] simframe=127 rng=1/2/3 animrng=28"] and got.dropped == 1   # the garbled frame is dropped and counted


def test_load_simid_cuts_a_splice_after_a_clean_token_and_drops_a_digit_splice(tmp_path):
    log = tmp_path / "x.log"
    log.write_text("[SIMID v1] simframe=1 rng=1/2/3 animrng=27 to VK_FORMAT_R8\n[SIMID v1] simframe=2 rng=1/2/3 animrng=2700061d\n", encoding="utf-8")
    got = sim.load_simid(str(log))
    assert got == ["[SIMID v1] simframe=1 rng=1/2/3 animrng=27"] and got.dropped == 1


def test_compare_aligns_by_simframe_and_reports_dropped():
    def mk(frames):
        return [f"[SIMID v1] simframe={i} t=0.0 n=1 graded=a visual=b strict=c nobj=1 objects=d nabl=1 abl=e rng=1/2/3 animrng=1" for i in frames]
    a, b = sim.SimidLines(mk(range(10))), sim.SimidLines(mk([0, 1, 2, 4, 5, 6, 7, 8, 9]))
    b.dropped = 1
    ok, msg = sim.compare(a, b, 5)
    assert ok and "dropped a=0 b=1" in msg


def _mk(frames, override=None):
    override = override or {}
    return sim.SimidLines(f"[SIMID v1] simframe={i} t=0.0 n=1 graded={override.get(i, 'a')} visual=b strict=c nobj=1 objects=d nabl=1 abl=e rng=1/2/3 animrng=1"
                          for i in frames)


def test_garbled_line_does_not_launder_a_conflicting_duplicate_tick():
    a, b = _mk(range(400)), _mk(range(400))
    a.append(_mk([50], {50: "zz"})[0])   # conflicting second observation of tick 50
    b.dropped = 1
    ok, msg = sim.compare(a, b, 300)
    assert not ok and msg.startswith("INCONCLUSIVE") and "simframe 50" in msg


def test_garbled_line_does_not_hide_599_common_ticks_of_600_required():
    a, b = _mk([i for i in range(601) if i != 10]), _mk([i for i in range(601) if i != 20])
    a.dropped = b.dropped = 1
    ok, msg = sim.compare(a, b, 300, required_length=600)
    assert not ok and msg.startswith("INCONCLUSIVE") and "599" in msg


def test_unexplained_gap_with_a_garbled_line_is_inconclusive():
    a, b = _mk([i for i in range(400) if i not in (10, 11)]), _mk([i for i in range(400) if i != 11])
    a.dropped = 1   # one garble cannot explain two holes, and tick 11 is in neither run
    ok, msg = sim.compare(a, b, 300)
    assert not ok and msg.startswith("INCONCLUSIVE")


def test_gap_in_run_b_is_validated_too():
    a, b = _mk(range(400)), _mk([i for i in range(400) if i != 30])
    a.dropped = 1   # the garble belongs to run a; run b's hole is unexplained
    ok, msg = sim.compare(a, b, 300)
    assert not ok and msg.startswith("INCONCLUSIVE") and "run b" in msg


def test_w127_garbled_tick_present_in_the_other_run_still_passes():
    a, b = _mk([i for i in range(400) if i != 77]), _mk(range(400))
    a.dropped = 1
    ok, msg = sim.compare(a, b, 300, required_length=399)
    assert ok, msg


def test_inconclusive_exits_nonzero_and_is_not_pass(tmp_path, capsys):
    la, lb = tmp_path / "a.log", tmp_path / "b.log"
    la.write_text("\n".join(_mk(range(400))) + "\n")
    lb.write_text("\n".join(_mk([i for i in range(400) if i not in (5, 6)])) + "\n[SIMID v1] simframe=9 garbled\n")
    rc = sim.main(["compare", str(la), str(lb)]) if "main" in dir(sim) else None
    out = capsys.readouterr().out
    assert rc != 0 and "PASS" not in out


_WIN_PASS = "[SMOKE v1] event=summary result=pass\n"


@pytest.mark.parametrize("rc,text,ok", [
    (0, "", True),
    (1, _WIN_PASS + "| mc2_03 | FAIL | perf_counter |\n", True),
    (1, _WIN_PASS + "| mc2_03 | FAIL | perf_counter, shader_error |\n", False),
    (1, _WIN_PASS + "| mc2_03 | FAIL | vk_validation_error |\n", False),
    (1, _WIN_PASS + "[runner] [DEPLOY_FINGERPRINT] MISMATCH\n| mc2_03 | FAIL | perf_counter |\n", False),
    (1, _WIN_PASS, False),
    (2, _WIN_PASS + "| mc2_03 | FAIL | perf_counter |\n", False),
])
def test_windowed_exit_ok_requires_a_successful_smoke(rc, text, ok):
    assert sim.windowed_exit_ok(rc, text) is ok


def _ticks(ticks):
    return sim.SimidLines([f"{sim.PREFIX}{i} t=0.000 n=2 graded=aa visual=bb strict=cc "
                           "nobj=3 objects=dd nabl=1 abl=ee rng=1/2/3 animrng=4" for i in ticks])


def test_windowed_pin_rejects_one_garbled_interior_tick_in_b():
    # p31 P0: A has ticks 1-800, B has 1-801 minus garbled tick 400: both reach 800 observations but only 799 are common.
    a, b = _ticks(range(1, 801)), _ticks([i for i in range(1, 802) if i != 400])
    b.dropped = 1
    ok, msg = sim.compare(a, b, 300, required_length=800)   # main() now passes the baseline's pinned frame count
    assert not ok and msg.startswith("INCONCLUSIVE") and "799" in msg


def test_windowed_pin_passes_complete_aligned_evidence():
    assert sim.compare(_ticks(range(1, 801)), _ticks(range(1, 801)), 300, required_length=800)[0]


def test_both_streams_are_validated_against_the_pin(tmp_path):
    good = list(_ticks(range(5)))
    bad = list(good)
    bad[3] = bad[3].replace("strict=cc", "strict=ff")
    path = baseline(tmp_path, good)
    assert sim.check_baseline_both(good, good, path)[0]
    ok, msg = sim.check_baseline_both(good, bad, path)
    assert not ok and msg.startswith("run b:")
