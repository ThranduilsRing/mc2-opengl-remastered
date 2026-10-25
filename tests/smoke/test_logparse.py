import re
from pathlib import Path
from scripts.smoke_lib.logparse import parse_log, LogSummary

FIX = Path(__file__).parent / "fixtures"

def test_mission_quit_records_runner_wallclock():
    s = parse_log("[TIMING v1] event=mission_quit elapsed_ms=53846.1\n", line_wallclocks=[54.2])
    assert s.mission_quit_ms == 53846.1
    assert s.mission_quit_wall_s == 54.2

def test_pass_log_counts_zero_faults():
    text = (FIX / "fake_engine_pass.log").read_text()
    # Synthesize per-line wallclocks at 0.1s intervals for this test.
    wall = [0.1 * i for i in range(len(text.splitlines()))]
    s = parse_log(text, line_wallclocks=wall)
    assert s.instr_banner_seen
    assert s.smoke_summary_result == "pass"
    assert s.pool_nulls == 0
    assert s.asset_oob == 0
    assert s.heartbeats_play > 0
    assert s.mission_ready_ms == 6800.1
    assert s.last_heartbeat_wall_s_play is not None
    assert s.perf.avg_fps == 59.8

def test_freeze_load_no_mission_ready():
    text = (FIX / "fake_engine_freeze_load.log").read_text()
    wall = [0.1 * i for i in range(len(text.splitlines()))]
    s = parse_log(text, line_wallclocks=wall)
    assert s.mission_ready_ms is None
    assert s.heartbeats_load >= 1
    assert s.heartbeats_play == 0
    assert s.smoke_summary_result is None
    assert s.last_heartbeat_wall_s_load is not None

def test_crash_no_summary_detected():
    log = (
        "[INSTR v1] enabled: smoke=1\n"
        "[TIMING v1] event=mission_ready elapsed_ms=5000\n"
        "CRASH: unhandled exception at 0xdeadbeef\n"
    )
    s = parse_log(log)
    assert s.crash_handler_hit is True
    assert s.smoke_summary_result is None


NATIVE_SHADER_LOADER = Path(__file__).resolve().parents[2] / "GameOS" / "gameos" / "vk_island_util.h"


def test_native_shader_loader_errors_are_counted_as_shader_failures():
    """Use every real loadSpv failure message so producer/parser drift fails closed."""
    src = NATIVE_SHADER_LOADER.read_text(encoding="utf-8")
    messages = re.findall(r'(?:log|later)\("([^"]+)"', src)
    assert len(messages) == 4
    for message in messages:
        line = message.replace("%s", "fixture.spv").replace("%ld", "2")
        assert parse_log("[VK_TEST] " + line + "\n").shader_errors == 1
    assert parse_log("[VK_TEST] loaded spv 'valid.spv'.\n").shader_errors == 0


def test_shader_failure_lines_count_once_per_line():
    log = "[SHADER ERROR] CompileShader: ERROR: 0:12: 'foo' : undeclared identifier\n[SHADER ERROR] Shader filename: a.frag, failed to load shader\n"
    assert parse_log(log).shader_errors == 2


def test_ordinary_shader_builder_info_is_not_a_failure():
    assert parse_log("Loading shader: gos_terrain.frag\nname: u_mvp type: mat4\n").shader_errors == 0


def test_crash_bundle_banner_counts_as_crash_handler_hit():
    log = (
        "[INSTR v1] enabled: smoke=1\n"
        "[TIMING v1] event=mission_ready elapsed_ms=5000\n"
        "MC2 Remastered Crash Report\n"
        "Code: 0xC0000374  Flags: 0x00000001  Addr: 00007FF9ABB2F499\n"
        "[CRASH] FAST-EXIT code=0xC0000374 bundle=X:\\crashes\\crash_x\n"
    )
    assert parse_log(log).crash_handler_hit is True
