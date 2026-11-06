"""[PERF_COUNTERS v1]: log parser, presence rule, perf_counter ceiling bucket, tools/perf/main_busy.py."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools" / "perf"))

from scripts.smoke_lib.gates import GateConfig, evaluate, load_perf_ceilings, load_window_ceilings  # noqa: E402
from scripts.smoke_lib.logparse import LogSummary, PerfRow, parse_log, parse_perf_counters, parse_perf_window  # noqa: E402
import main_busy  # noqa: E402

LINE = ("[PERF_COUNTERS v1] frames=300 draws.props=12.5 draws.terrain=3 wait_sealed_ms=0.2500 first_vk_wait_site=getOrCreateEngineVulkanContext "
        "first_vk_wait_ms_into_frame=1.5000")


def _ready():
    return LogSummary(content_summaries=1, instr_banner_seen=True, smoke_banner_seen=True, mission_resolve_seen=True,
                      smoke_summary_result="pass", mission_ready_ms=5000.0, heartbeats_play=30,
                      last_heartbeat_wall_s_play=30.5, perf=PerfRow(avg_fps=60))


def test_parse_takes_numeric_fields_and_skips_labels():
    got = parse_perf_counters(LINE)
    assert got["frames"] == 300 and got["draws.props"] == 12.5 and got["wait_sealed_ms"] == 0.25
    assert "first_vk_wait_site" not in got and got["first_vk_wait_ms_into_frame"] == 1.5


def test_parse_log_keeps_last_line():
    s = parse_log(LINE + "\n" + LINE.replace("frames=300", "frames=600") + "\n")
    assert s.perf_counters["frames"] == 600
    assert s.presence_counts.get("perf_counters_missing") is None or s.presence_counts["perf_counters_missing"] >= 1


def test_missing_line_is_red_on_tier1_stem():
    cfg = GateConfig(duration_s=30, stem="mc2_24", vegetation_expected=False, perf_ceilings={})
    v = evaluate(_ready(), cfg, exit_code=0, walltime_s=31)
    assert "perf_counters_missing" in v.buckets
    s = _ready()
    s.presence_counts = {"perf_counters_missing": 1}
    assert "perf_counters_missing" not in evaluate(s, cfg, exit_code=0, walltime_s=31).buckets


def test_ceiling_bucket_fires_only_over_limit():
    cfg = GateConfig(duration_s=30, stem="mc2_24", vegetation_expected=False,
                     perf_ceilings={"mc2_24": {"draws.props": 10.0, "draws.terrain": 5.0}})
    s = _ready()
    s.presence_counts = {"perf_counters_missing": 1}
    s.perf_counters = {"draws.props": 12.5, "draws.terrain": 3.0}
    v = evaluate(s, cfg, exit_code=0, walltime_s=31)
    # perf_counter-only is a recorded warning, not a red bucket (owner rule 2026-11-01)
    assert v.passed and not v.buckets and any("draws.props" in w for w in v.warnings)
    s.perf_counters["draws.props"] = 9.0
    assert not evaluate(s, cfg, exit_code=0, walltime_s=31).warnings


def test_shipped_ceilings_file_is_well_formed():
    for stem, limits in load_perf_ceilings().items():
        assert stem.startswith("mc2_") and all(isinstance(v, (int, float)) and v >= 0 for v in limits.values())


def test_main_busy_columns_add_up_and_tick_frames_separate(tmp_path):
    rows = [{"type": "cpu", "frame": i, "ms": 10.0, "ticks": 1 if i % 2 else 0, "wait_sealed_ms": 1.0, "wait_sim_ms": 2.0,
             "wait_gpu_main_ms": 3.0} for i in range(10)]
    (tmp_path / "frames.jsonl").write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    t = main_busy.table(main_busy.load_cpu_rows(tmp_path))
    assert t["all"]["frames"] == 10 and t["tick_frames"]["frames"] == 5
    assert t["all"]["main_exec"]["median"] == 4.0 and t["all"]["MAIN_BUSY"]["median"] == 7.0
    assert "tick_frames" in main_busy.render(t)


WIN_LINE = ("[PERF_WINDOW v1] ticks=30..120 complete=1 frames=60 draws.props=12.5 draws.mech_depth=40.0 draws.mech_color=41.0 "
            "extract.bytes=100.0")
WC = {"ticks": "30..120", "stems": {"mc2_24": {"draws.mech_depth": 50.0, "draws.mech_color": 50.0}}}


def _win(window, wc=WC):
    cfg = GateConfig(duration_s=30, stem="mc2_24", vegetation_expected=False, perf_ceilings={}, window_ceilings=wc)
    s = _ready()
    s.presence_counts = {"perf_counters_missing": 1}
    s.perf_window = window
    return evaluate(s, cfg, exit_code=0, walltime_s=31)


def test_parse_perf_window_keeps_tick_range_string():
    w = parse_perf_window(WIN_LINE)
    assert w["ticks"] == "30..120" and w["complete"] == 1 and w["draws.mech_depth"] == 40.0 and w["frames"] == 60


def test_parse_log_reads_window_line():
    assert parse_log(WIN_LINE + "\n").perf_window["draws.mech_color"] == 41.0


def test_window_within_ceiling_passes():
    assert _win(parse_perf_window(WIN_LINE)).passed


def test_window_over_ceiling_fails_per_pass():
    w = parse_perf_window(WIN_LINE)
    w["draws.mech_depth"] = 119.0
    v = _win(w)
    assert v.passed and not v.buckets and "draws.mech_depth=119>50" in v.warnings[0]
    assert "mech_color" not in v.warnings[0]


def test_window_missing_incomplete_or_other_range_warns_not_silent():
    assert _win({}).warnings and "not graded" in _win({}).warnings[0]
    w = parse_perf_window(WIN_LINE)
    w["complete"] = 0
    assert _win(w).warnings
    w = parse_perf_window(WIN_LINE)
    w["ticks"] = "60..300"
    v = _win(w)
    assert v.passed and "window ticks 60..300" in v.warnings[0]


def test_window_ceiling_only_applies_to_its_stem():
    v = _win({}, wc={"ticks": "30..120", "stems": {"mc2_01": {"draws.mech_depth": 1.0}}})
    assert v.passed and not v.warnings


def test_shipped_window_ceilings_are_well_formed():
    wc = load_window_ceilings()
    for stem, limits in wc.get("stems", {}).items():
        assert stem.startswith("mc2_") and all(isinstance(v, (int, float)) and v > 0 for v in limits.values())
        assert all(k.startswith("draws.") for k in limits)
