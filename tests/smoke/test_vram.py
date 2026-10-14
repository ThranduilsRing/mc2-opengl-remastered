"""The `[VRAM v1]` line (GameOS/gameos/gpu_memory_report.cpp) and how the smoke parser and report read it (scripts/smoke_lib/vram.py)."""
from scripts.smoke_lib import logparse, report, vram
from scripts.smoke_lib.report import Row

GL_AMD = ("[VRAM v1] phase=load_end backend=gl gl_ati_tex_free_mb=21000 gl_ati_tex_largest_free_mb=20000 gl_nvx_dedicated_mb=-1 "
          "gl_nvx_available_mb=-1 vk_budget_mb=-1 vk_usage_mb=-1 vk_allocated_mb=-1 adapter_dedicated_mb=24560 proc_dedicated_mb=3100 proc_budget_mb=23000")
VK = ("[VRAM v1] phase=run_end backend=vk gl_ati_tex_free_mb=-1 gl_ati_tex_largest_free_mb=-1 gl_nvx_dedicated_mb=-1 gl_nvx_available_mb=-1 "
      "vk_budget_mb=23000 vk_usage_mb=4200 vk_allocated_mb=4000 adapter_dedicated_mb=24560 proc_dedicated_mb=-1 proc_budget_mb=-1")
GL_NVIDIA = ("[VRAM v1] phase=load_end backend=gl gl_ati_tex_free_mb=-1 gl_ati_tex_largest_free_mb=-1 gl_nvx_dedicated_mb=12288 "
             "gl_nvx_available_mb=8000 vk_budget_mb=-1 vk_usage_mb=-1 vk_allocated_mb=-1 adapter_dedicated_mb=-1 proc_dedicated_mb=-1 proc_budget_mb=-1")
NOTHING = ("[VRAM v1] phase=run_end backend=gl gl_ati_tex_free_mb=-1 gl_ati_tex_largest_free_mb=-1 gl_nvx_dedicated_mb=-1 gl_nvx_available_mb=-1 "
           "vk_budget_mb=-1 vk_usage_mb=-1 vk_allocated_mb=-1 adapter_dedicated_mb=-1 proc_dedicated_mb=-1 proc_budget_mb=-1")


def test_a_line_is_parsed_by_field_name():
    (s,) = vram.parse("noise before\n" + GL_AMD + "\nnoise after")
    assert (s.phase, s.backend) == ("load_end", "gl")
    assert s.get("proc_dedicated_mb") == 3100 and s.get("adapter_dedicated_mb") == 24560 and s.get("gl_ati_tex_free_mb") == 21000
    assert s.get("vk_usage_mb") is None            # -1 = the counter does not exist on this driver
    assert s.get("not_a_field") is None


def test_a_later_field_added_at_the_end_does_not_break_the_reader():
    (s,) = vram.parse(GL_AMD + " some_future_mb=7")
    assert s.get("some_future_mb") == 7 and s.get("proc_dedicated_mb") == 3100


def test_truncated_and_foreign_lines_are_skipped():
    assert vram.parse("[VRAM v1] phase=load_end backend=gl gl_ati_tex_fr") == []
    assert vram.parse("[VRAM v2] phase=load_end backend=gl") == []
    assert vram.parse("") == []


def test_used_memory_prefers_the_process_counter_then_the_backend_counter():
    assert vram.used_mb(vram.parse(GL_AMD)[0]) == (3100, "dxgi-process")
    assert vram.used_mb(vram.parse(VK)[0]) == (4200, "vma-usage")
    assert vram.used_mb(vram.parse(GL_NVIDIA)[0]) == (4288, "nvx-dedicated-minus-available")
    assert vram.used_mb(vram.parse(NOTHING)[0]) == (None, "none")
    amd_no_process = GL_AMD.replace("proc_dedicated_mb=3100", "proc_dedicated_mb=-1")
    assert vram.used_mb(vram.parse(amd_no_process)[0]) == (24560 - 21000, "adapter-minus-gl-free")


def test_describe_says_what_it_read_or_that_it_read_nothing():
    assert vram.describe(vram.parse(GL_AMD)[0]) == "load_end gl: 3100 MiB in use (dxgi-process), budget 23000 MiB"
    assert vram.describe(vram.parse(NOTHING)[0]) == "run_end gl: no GPU memory counter available"


def test_the_log_parser_collects_the_samples_of_a_run():
    summary = logparse.parse_log("[SMOKE v1] event=banner\n" + GL_AMD + "\n[HEARTBEAT] frames=1 elapsed_ms=2\n" + VK + "\n")
    assert [(v.phase, v.backend) for v in summary.vram] == [("load_end", "gl"), ("run_end", "vk")]


def test_the_report_carries_a_vram_line_and_the_json_rows(tmp_path):
    from scripts.smoke_lib.logparse import LogSummary
    from scripts.smoke_lib.runner import Verdict
    summary = logparse.parse_log(GL_AMD + "\n" + VK + "\n")
    row = Row(stem="mc2_01", verdict=Verdict(passed=True, buckets=[], details=[]), summary=summary, destroys_delta=0)
    md = report.render_markdown([row], tier="tier1", profile="stock", timestamp="2026-10-13T00-00-00")
    assert "**VRAM:** mc2_01 load_end gl: 3100 MiB in use (dxgi-process), budget 23000 MiB; run_end vk: 4200 MiB in use (vma-usage)" in md
    js = report.render_json([row], tier="tier1", profile="stock", timestamp="2026-10-13T00-00-00")
    assert js["rows"][0]["vram"][1]["values"]["vk_usage_mb"] == 4200
    quiet = Row(stem="mc2_03", verdict=Verdict(passed=True, buckets=[], details=[]), summary=LogSummary(), destroys_delta=0)
    assert "VRAM" not in report.render_markdown([quiet], tier="tier1", profile="stock", timestamp="t")
