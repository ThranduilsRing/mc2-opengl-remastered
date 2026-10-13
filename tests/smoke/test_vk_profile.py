"""The `[VK_PROFILE v1]` line (GameOS/gameos/vk_device_profile.h) and how the smoke parser grades it (scripts/smoke_lib/vk_profile.py)."""
import re
from pathlib import Path

from scripts.smoke_lib import logparse, report, vk_profile
from scripts.smoke_lib.report import Row

ROOT = Path(__file__).resolve().parents[2]

OK_LINE = ('[VK_PROFILE v1] verdict=ok device="AMD Radeon RX 7900 XTX" api=1.4.0 instance_api=1.3.0 floor=1.2 required_missing=- wanted_missing=- '
           "items=api_1_2:core,swapchain:ext,limits:limit,timeline_semaphore:core,buffer_device_address:core,descriptor_indexing:core,"
           "dynamic_rendering:core,synchronization2:core,independent_blend:core,memory_budget:ext enabled=timelineSemaphore,bufferDeviceAddress,descriptorIndexing,"
           "runtimeDescriptorArray,shaderSampledImageArrayNonUniformIndexing,descriptorBindingPartiallyBound,dynamicRendering,synchronization2,independentBlend "
           "extensions=VK_KHR_swapchain,VK_EXT_memory_budget")
OLD_GPU = ('[VK_PROFILE v1] verdict=refused device="old gpu" api=1.1.0 instance_api=1.3.0 floor=1.2 '
           "required_missing=api_1_2,timeline_semaphore,buffer_device_address,descriptor_indexing,dynamic_rendering,synchronization2,independent_blend wanted_missing=- "
           "items=api_1_2:-,swapchain:ext,limits:limit,timeline_semaphore:-,buffer_device_address:-,descriptor_indexing:-,dynamic_rendering:-,"
           "synchronization2:-,independent_blend:-,memory_budget:ext enabled=- extensions=-")
EXT_ROUTE = OK_LINE.replace("api=1.4.0 instance_api=1.3.0", "api=1.2.0 instance_api=1.2.0").replace("dynamic_rendering:core,synchronization2:core",
                                                                                                    "dynamic_rendering:ext,synchronization2:ext")


def test_a_line_is_parsed_into_its_fields():
    (p,) = vk_profile.parse("noise\n" + OK_LINE + "\nnoise")
    assert p.verdict == "ok" and p.accepted and p.device == "AMD Radeon RX 7900 XTX"
    assert (p.api, p.instance_api, p.floor) == ("1.4.0", "1.3.0", "1.2")
    assert p.required_missing == [] and p.wanted_missing == []
    assert p.routes["dynamic_rendering"] == "core" and p.routes["swapchain"] == "ext" and p.routes["limits"] == "limit"
    assert "timelineSemaphore" in p.enabled and p.extensions == ["VK_KHR_swapchain", "VK_EXT_memory_budget"]
    assert p.routes["memory_budget"] == "ext"


def test_an_extension_route_on_a_1_2_device_is_read_as_ext():
    (p,) = vk_profile.parse(EXT_ROUTE)
    assert p.routes["dynamic_rendering"] == "ext" and p.api == "1.2.0"


def test_a_refused_line_names_what_is_missing():
    (p,) = vk_profile.parse(OLD_GPU)
    assert not p.accepted and p.required_missing[0] == "api_1_2" and len(p.required_missing) == 7
    assert p.wanted_missing == [] and p.enabled == [] and p.extensions == []


def test_a_missing_facility_of_the_ruling_refuses_the_device():
    """Dynamic rendering, synchronization2, timeline semaphores, descriptor indexing and buffer device address are required (owner ruling 2026-10-13)."""
    for facility in ("timeline_semaphore", "buffer_device_address", "descriptor_indexing", "dynamic_rendering", "synchronization2", "independent_blend"):
        line = (OK_LINE.replace("verdict=ok", "verdict=refused").replace("required_missing=-", f"required_missing={facility}")
                .replace(f"{facility}:core", f"{facility}:-").replace("enabled=timelineSemaphore", "enabled=-").replace("extensions=VK_KHR_swapchain,VK_EXT_memory_budget", "extensions=-"))
        (p,) = vk_profile.parse(line)
        assert not p.accepted and p.required_missing == [facility]
        (problem,) = vk_profile.grade([p])
        assert "REFUSED" in problem and facility in problem and "fell back to GL" in problem


def test_a_truncated_or_foreign_line_is_skipped():
    assert vk_profile.parse(OK_LINE[:80]) == []
    assert vk_profile.parse(OK_LINE.replace("v1]", "v2]")) == []
    assert vk_profile.parse("") == []


def test_grading_is_red_for_a_refused_verdict_and_for_a_contradiction_but_not_for_missing_wanted_items():
    assert vk_profile.grade(vk_profile.parse(OK_LINE)) == []
    wanted_only = OK_LINE.replace("wanted_missing=-", "wanted_missing=memory_budget")
    assert vk_profile.grade(vk_profile.parse(wanted_only)) == []         # an optional item (the memory-budget extension) is reported, never a failure
    refused = vk_profile.grade(vk_profile.parse(OLD_GPU))
    assert len(refused) == 1 and "REFUSED" in refused[0] and "api_1_2" in refused[0] and "fell back to GL" in refused[0]
    contradiction = OK_LINE.replace("required_missing=-", "required_missing=limits")
    assert "lists required item(s) missing" in vk_profile.grade(vk_profile.parse(contradiction))[0]


def test_the_log_parser_collects_the_profile_and_counts_the_refusal_as_a_red_marker():
    summary = logparse.parse_log(OK_LINE + "\n" + OLD_GPU + "\n")
    assert [p.verdict for p in summary.vk_profile] == ["ok", "refused"]
    assert summary.marker_counts.get("vk_profile_refused") == 1
    clean = logparse.parse_log(OK_LINE + "\n")
    assert clean.marker_counts.get("vk_profile_refused", 0) == 0
    row = next(mb for mb in logparse.MARKER_BUCKETS if mb.bucket == "vk_profile_refused")
    assert row.severity == "red"


def test_a_refused_profile_makes_the_mission_verdict_red():
    from scripts.smoke_lib.gates import GateConfig, evaluate
    cfg = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3, duration_s=30)

    def run(text):
        s = logparse.parse_log(text)
        s.instr_banner_seen = s.smoke_banner_seen = s.mission_resolve_seen = True
        s.smoke_summary_result, s.mission_ready_ms, s.heartbeats_play, s.last_heartbeat_wall_s_play = "pass", 5000.0, 30, 30.5
        s.perf = logparse.PerfRow(avg_fps=60)
        return evaluate(s, cfg, exit_code=0, walltime_s=31)

    assert run(OK_LINE + "\n").passed
    refused = run(OLD_GPU + "\n")
    assert not refused.passed and "vk_profile_refused" in refused.buckets


def test_the_report_has_a_vulkan_profile_line():
    from scripts.smoke_lib.logparse import LogSummary
    from scripts.smoke_lib.runner import Verdict
    summary = logparse.parse_log(OK_LINE + "\n")
    row = Row(stem="mc2_01", verdict=Verdict(passed=True, buckets=[], details=[]), summary=summary, destroys_delta=0)
    md = report.render_markdown([row], tier="tier1", profile="stock", timestamp="2026-10-13T00-00-00")
    assert "**Vulkan profile:** ok AMD Radeon RX 7900 XTX api 1.4.0 (instance 1.3.0)" in md and "wanted missing: -" in md
    quiet = Row(stem="mc2_03", verdict=Verdict(passed=True, buckets=[], details=[]), summary=LogSummary(), destroys_delta=0)
    assert "Vulkan profile" not in report.render_markdown([quiet], tier="tier1", profile="stock", timestamp="t")


def test_the_python_table_matches_the_header_it_reads():
    """The item ids the engine prints are the ones a reader of this parser expects: one list in the header, checked here against the doctest's line."""
    header = (ROOT / "GameOS" / "gameos" / "vk_device_profile.h").read_text(encoding="utf-8")
    ids = re.search(r"kItemIds\[\] = \{(.*?)\};", header, re.S).group(1)
    engine_ids = re.findall(r'"(\w+)"', ids)
    (p,) = vk_profile.parse(OK_LINE)
    assert list(p.routes) == engine_ids
