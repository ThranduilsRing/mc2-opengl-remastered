from scripts.smoke_lib.logparse import LogSummary, PerfRow
from scripts.smoke_lib.gates import Verdict
from scripts.smoke_lib.report import Row, render_markdown, render_json
import pytest

def test_markdown_contains_header_and_rows():
    rows = [
        Row(stem="mc2_01", verdict=Verdict(passed=True), summary=LogSummary(
            perf=PerfRow(avg_fps=142, p1low_fps=58), mission_ready_ms=4300),
            destroys_delta=+2),
        Row(stem="mc2_10", verdict=Verdict(passed=False, buckets=["gl_error"]),
            summary=LogSummary(perf=PerfRow(avg_fps=119, p1low_fps=32),
                               mission_ready_ms=6800), destroys_delta=0),
    ]
    md = render_markdown(rows, tier="tier1", profile="stock",
                         timestamp="2026-04-23T14-32-07")
    assert "# Smoke run" in md
    assert "| mc2_01  | PASS" in md
    assert "| mc2_10  | FAIL" in md
    assert "gl_error" in md

def test_json_roundtrips_verdict():
    rows = [Row(stem="mc2_01", verdict=Verdict(passed=True),
                summary=LogSummary(perf=PerfRow(avg_fps=60)), destroys_delta=0)]
    d = render_json(rows, tier="tier1", profile="stock", timestamp="t")
    assert d["tier"] == "tier1"
    assert d["rows"][0]["stem"] == "mc2_01"
    assert d["rows"][0]["result"] == "PASS"


@pytest.mark.parametrize("mode,description", [
    ("head", "worktree HEAD abc123def456"),
    ("commit", "--ref-commit abc123def456 resolved=abc123def456 (clean commit)"),
    ("exe", "--ref-exe pre.exe sha256=" + "a" * 64 + " (byte identity)")])
@pytest.mark.parametrize("bad", [False, True])
def test_report_records_reference_and_verdict_text(mode, description, bad):
    row = Row(stem="mc2_01", verdict=Verdict(passed=True), summary=LogSummary())
    line = "[DEPLOY_FINGERPRINT] " + ("MISMATCH: wrong reference" if bad else "OK: identity verified")
    record = {"reference": {"mode": mode}, "description": description,
              "required": True, "hard_fail": bad,
              "checks": [{"mission": "mc2_01", "hard_fail": bad, "lines": [line]}]}
    kw = dict(tier="tier1", profile="stock", timestamp="t", fingerprint=record,
              run_failures=["DEPLOY_FINGERPRINT: " + description] if bad else [])
    md = render_markdown([row], **kw)
    js = render_json([row], **kw)
    want = "FAIL" if bad else "PASS"
    assert "result=" + want in md
    assert f"**DEPLOY_FINGERPRINT:** {want} (required)" in md
    assert description in md and line in md
    assert js["overall_result"] == want
    assert js["fingerprint"] == record


def test_report_does_not_claim_fingerprint_pass_when_check_unavailable():
    row = Row(stem="mc2_01", verdict=Verdict(passed=True), summary=LogSummary())
    record = {"reference": None, "description": "worktree HEAD (module unavailable)",
              "required": False, "hard_fail": False, "checks": []}
    md = render_markdown([row], tier="tier1", profile="stock", timestamp="t",
                         fingerprint=record)
    assert "**DEPLOY_FINGERPRINT:** NOT VERIFIED (gate disabled)" in md
