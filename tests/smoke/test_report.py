from scripts.smoke_lib.logparse import LogSummary, PerfRow
from scripts.smoke_lib.gates import Verdict
from scripts.smoke_lib.report import Row, render_markdown, render_json

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
