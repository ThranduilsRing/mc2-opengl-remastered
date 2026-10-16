"""The VRAM section of tools/perf_baseline_report.py: the Tracy plot over the steady window plus the [VRAM v1] lines of the game log."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("perf_baseline_report_under_test", ROOT / "tools" / "perf_baseline_report.py")
pbr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pbr)

LOG = ("noise\n[VRAM v1] phase=load_end backend=gl "
       "vk_budget_mb=-1 vk_usage_mb=-1 vk_allocated_mb=-1 adapter_dedicated_mb=24560 proc_dedicated_mb=3100 proc_budget_mb=23000\n")


def test_the_plot_is_summarised_over_the_steady_window_only():
    plot = [(1_000, 1000.0), (2_000, 2000.0), (5_000, 3000.0), (6_000, 3200.0), (7_000, 3100.0)]
    lines = pbr.vram_lines(plot, 4_000)
    assert "3 sample(s) in the steady window" in lines[0] and "median 3100 MiB" in lines[0] and "max 3200 MiB" in lines[0]
    assert "first 1000 MiB" in lines[0] and "last 3100 MiB" in lines[0]


def test_a_trace_without_the_plot_says_so_instead_of_staying_silent():
    lines = pbr.vram_lines([], 0)
    assert "no `VRAM.Process.MB` plot" in lines[0]


def test_the_game_log_samples_are_described():
    lines = pbr.vram_lines([(10, 3000.0)], 0, LOG)
    assert any("game log: load_end gl: 3100 MiB in use (dxgi-process)" in l for l in lines)
