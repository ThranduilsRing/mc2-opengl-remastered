"""[PERF_COUNTERS v1]: log parser, presence rule, perf_counter ceiling bucket, tools/perf/main_busy.py."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools" / "perf"))

from scripts.smoke_lib.gates import GateConfig, evaluate, load_perf_ceilings  # noqa: E402
from scripts.smoke_lib.logparse import LogSummary, PerfRow, parse_log, parse_perf_counters  # noqa: E402
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
