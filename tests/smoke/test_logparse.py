import re
from pathlib import Path
from scripts.smoke_lib.logparse import parse_log, LogSummary

FIX = Path(__file__).parent / "fixtures"

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


SHADER_BUILDER = Path(__file__).resolve().parents[2] / "GameOS" / "gameos" / "utils" / "shader_builder.cpp"


def test_shader_builder_error_token_is_counted_as_a_shader_failure():
    """The producer's own prefix (read from shader_builder.cpp) is what the shader-failure bucket matches: a drifted prefix fails here."""
    src = SHADER_BUILDER.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'#define SHADER_LOG_ERROR\(\.\.\.\) MC2_LOG\(Shader, Error, "([^"]*)" __VA_ARGS__\)', src)
    assert m, "shader_builder.cpp no longer defines SHADER_LOG_ERROR as MC2_LOG(Shader, Error, \"<prefix>\" __VA_ARGS__)"
    for body in ("Type mismatch: u_foo", "Duplicate shader name: gos_x.frag", "[SPIRV] failed to read a.spv; GLSL fallback"):
        assert parse_log(m.group(1) + body + "\n").shader_errors == 1


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
