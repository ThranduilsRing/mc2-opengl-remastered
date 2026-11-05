"""The `[VK_VALIDATION v1]` line (GameOS/gameos/vulkan_context.cpp) and how the smoke parser grades it (scripts/smoke_lib/vk_validation.py)."""
from scripts.smoke_lib import logparse, report, vk_validation
from scripts.smoke_lib.report import Row

LAYER_ON = '[VK_VALIDATION v1] severity=info id=layer_enabled text="VK_LAYER_KHRONOS_validation messenger engaged, preset=core"'
CANARY = '[VK_VALIDATION v1] severity=info id=canary_fired text="VUID-VkBufferCreateInfo-size-00912"'
ENABLED = LAYER_ON + "\n" + CANARY         # a confirmed run: the layer is up AND its deliberate canary was reported
ERROR = ('[VK_VALIDATION v1] severity=error id=VUID-vkCmdDraw-None-02699 text="vkCmdDraw(): descriptor set 0 binding 1 was not updated"')
ERROR_AGAIN = ERROR.replace("binding 1", "binding 2")
WARNING = '[VK_VALIDATION v1] severity=warning id=UNASSIGNED-CoreValidation-Shader-OutputNotConsumed text="fragment output 3 is never consumed"'
UNAVAILABLE = ('[VK_VALIDATION v1] severity=error id=layer_unavailable text="VK_LAYER_KHRONOS_validation was requested (MC2_VULKAN_VALIDATION) '
               'and the Vulkan loader does not offer it; this run is NOT validated"')


def test_a_line_is_parsed_into_severity_id_and_text():
    (m,) = vk_validation.parse("noise\n" + ERROR + "\nnoise")
    assert (m.severity, m.id) == ("error", "VUID-vkCmdDraw-None-02699")
    assert m.text == "vkCmdDraw(): descriptor set 0 binding 1 was not updated"


def test_a_truncated_or_foreign_line_is_skipped():
    assert vk_validation.parse(ERROR[:50]) == []
    assert vk_validation.parse(ERROR.replace("v1]", "v2]")) == []
    assert vk_validation.parse("[VK_ENGINE_CONTEXT_VALIDATION] ERROR: old style") == []
    assert vk_validation.parse("") == []


def test_errors_and_warnings_are_counted_per_id():
    s = vk_validation.summarize(vk_validation.parse("\n".join([ENABLED, ERROR, ERROR_AGAIN, WARNING, WARNING])))
    assert (s.errors, s.warnings) == (2, 2)
    assert dict(s.error_ids) == {"VUID-vkCmdDraw-None-02699": 2}
    assert s.distinct_ids == ["VUID-vkCmdDraw-None-02699", "UNASSIGNED-CoreValidation-Shader-OutputNotConsumed"]


def test_only_errors_grade_red_and_each_distinct_id_is_one_reason():
    assert vk_validation.grade(vk_validation.parse(ENABLED + "\n" + WARNING)) == []
    reasons = vk_validation.grade(vk_validation.parse("\n".join([ERROR, ERROR_AGAIN, UNAVAILABLE])))
    assert reasons == ["Vulkan validation error VUID-vkCmdDraw-None-02699 x2", "Vulkan validation error layer_unavailable x1"]


def test_describe_says_whether_the_layer_was_confirmed_on():
    clean = vk_validation.parse(ENABLED)
    assert vk_validation.describe(clean) == "errors=0 warnings=0 canary=fired"
    dirty = vk_validation.parse("\n".join([ENABLED, ERROR, WARNING]))
    assert vk_validation.describe(dirty) == ("errors=1 (VUID-vkCmdDraw-None-02699 x1) "
                                             "warnings=1 (UNASSIGNED-CoreValidation-Shader-OutputNotConsumed x1) canary=fired")
    assert "layer not confirmed on" in vk_validation.describe(vk_validation.parse(WARNING))


def test_the_log_parser_collects_the_messages_and_counts_errors_as_a_red_marker():
    summary = logparse.parse_log("\n".join([ENABLED, WARNING, ERROR, ERROR_AGAIN]) + "\n")
    assert [m.severity for m in summary.vk_validation] == ["info", "info", "warning", "error", "error"]
    assert summary.marker_counts.get("vk_validation_error") == 2           # the warning and the info line are not red
    row = next(mb for mb in logparse.MARKER_BUCKETS if mb.bucket == "vk_validation_error")
    assert row.severity == "red"
    clean = logparse.parse_log(ENABLED + "\n" + WARNING + "\n")
    assert clean.marker_counts.get("vk_validation_error", 0) == 0


def test_a_requested_but_missing_layer_makes_the_mission_red():
    assert "vk_validation_error" in logparse.parse_log(UNAVAILABLE + "\n").marker_counts


def _verdict(text):
    from scripts.smoke_lib.gates import GateConfig, evaluate
    cfg = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3, duration_s=30)
    s = logparse.parse_log(text)
    s.content_summaries = 1
    s.instr_banner_seen = s.smoke_banner_seen = s.mission_resolve_seen = True
    s.smoke_summary_result, s.mission_ready_ms, s.heartbeats_play, s.last_heartbeat_wall_s_play = "pass", 5000.0, 30, 30.5
    s.perf = logparse.PerfRow(avg_fps=60)
    return evaluate(s, cfg, exit_code=0, walltime_s=31)


def test_an_error_line_fails_the_mission_and_warnings_do_not():
    assert _verdict(ENABLED + "\n" + WARNING + "\n").passed
    red = _verdict(ENABLED + "\n" + ERROR + "\n")
    assert not red.passed and "vk_validation_error" in red.buckets


def test_the_report_has_a_vulkan_validation_line_only_for_missions_that_printed_one():
    from scripts.smoke_lib.logparse import LogSummary
    from scripts.smoke_lib.runner import Verdict
    summary = logparse.parse_log("\n".join([ENABLED, ERROR, WARNING]) + "\n")
    row = Row(stem="mc2_01", verdict=Verdict(passed=False, buckets=["vk_validation_error"], details=[]), summary=summary, destroys_delta=0)
    md = report.render_markdown([row], tier="tier1", profile="stock", timestamp="2026-10-13T00-00-00")
    assert "**Vulkan validation:** mc2_01 errors=1 (VUID-vkCmdDraw-None-02699 x1) warnings=1" in md
    quiet = Row(stem="mc2_03", verdict=Verdict(passed=True, buckets=[], details=[]), summary=LogSummary(), destroys_delta=0)
    assert "Vulkan validation" not in report.render_markdown([quiet], tier="tier1", profile="stock", timestamp="t")


def test_a_run_that_asked_for_validation_and_never_confirmed_the_layer_is_red():
    assert vk_validation.grade(vk_validation.parse(ENABLED), asked=True) == []
    assert vk_validation.grade(vk_validation.parse(""), asked=True) == [vk_validation.UNCONFIRMED]
    assert vk_validation.grade(vk_validation.parse(""), asked=False) == []


def test_requested_reads_the_environment_the_way_the_engine_does():
    assert vk_validation.requested({"MC2_VULKAN_VALIDATION": "1"})
    assert vk_validation.requested({"MC2_VULKAN_VALIDATION": ""})            # set but empty is the core preset
    assert vk_validation.requested({"MC2_VULKAN_VALIDATION": "sync"})
    assert not vk_validation.requested({"MC2_VULKAN_VALIDATION": "0"})
    assert not vk_validation.requested({"MC2_VULKAN_VALIDATION": "off"})
    assert not vk_validation.requested({})


def test_the_gate_grades_an_unconfirmed_request_red_and_a_confirmed_one_clean():
    from scripts.smoke_lib.gates import GateConfig, evaluate
    cfg = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3, duration_s=30, validation_requested=True)

    def run(text):
        s = logparse.parse_log(text)
        s.content_summaries = 1
        s.instr_banner_seen = s.smoke_banner_seen = s.mission_resolve_seen = True
        s.smoke_summary_result, s.mission_ready_ms, s.heartbeats_play, s.last_heartbeat_wall_s_play = "pass", 5000.0, 30, 30.5
        s.perf = logparse.PerfRow(avg_fps=60)
        return evaluate(s, cfg, exit_code=0, walltime_s=31)

    assert run(ENABLED + "\n").passed
    unconfirmed = run("\n")
    assert not unconfirmed.passed and "vk_validation_unconfirmed" in unconfirmed.buckets


SYNC = ('[VK_VALIDATION v1] severity=error id=SYNC-HAZARD-WRITE_AFTER_WRITE text="obj=10:0x1:shadow.bake.mask | vkCmdCopyImage WRITE_AFTER_WRITE"')


def test_a_layer_without_the_canary_is_not_confirmed():
    assert vk_validation.layer_enabled(vk_validation.parse(LAYER_ON)) and not vk_validation.confirmed(vk_validation.parse(LAYER_ON))
    assert vk_validation.grade(vk_validation.parse(LAYER_ON), asked=True) == [vk_validation.UNCONFIRMED]
    assert "canary=MISSING" in vk_validation.describe(vk_validation.parse(LAYER_ON))
    assert "canary=fired" in vk_validation.describe(vk_validation.parse(ENABLED))


def test_a_known_unexpired_entry_is_not_red_and_an_expired_or_unlisted_one_is():
    from datetime import date
    entries = vk_validation.known_entries(today=date(2026, 11, 6))
    assert vk_validation.is_known("SYNC-HAZARD-WRITE_AFTER_WRITE", "x shadow.bake.mask y", entries)
    assert not vk_validation.is_known("SYNC-HAZARD-WRITE_AFTER_WRITE", "some other image", entries)
    assert not vk_validation.is_known("VUID-vkCmdDraw-None-02699", "shadow.bake.mask", entries)
    assert vk_validation.known_entries(today=date(2027, 1, 1)) == []           # the review window ran out: red again
    s = logparse.parse_log("\n".join([ENABLED, SYNC, ERROR]) + "\n")
    assert s.marker_counts.get("vk_validation_error") in (1, 2)                # the known hit leaves at most the unlisted VUID (the entry may have expired)


def test_a_raw_layer_error_line_is_red_unless_known():
    raw = "Validation Error: [ VUID-vkCmdDraw-None-02699 ] Object 0: handle = 0x1 | bad"
    assert logparse.parse_log(raw + "\n").marker_counts.get("vk_validation_raw") == 1
    assert "vk_validation_raw" not in logparse.parse_log(ERROR.replace('"vkCmd', '"Validation Error: [ VUID-x ] vkCmd') + "\n").marker_counts
