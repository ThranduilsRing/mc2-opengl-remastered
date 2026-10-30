"""The stock content gate must observe zero, errors, and missing evidence."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib.logparse import parse_log
from smoke_lib.gates import GateConfig, evaluate
from smoke_lib import report
from smoke_lib.gates import Verdict


def buckets(text, budget=0):
    return evaluate(parse_log(text), GateConfig(content_error_budget=budget),
                    exit_code=0, walltime_s=20).buckets


def test_stock_zero():
    text = "[CONTENT_SUMMARY] mission=mc2_03 total=0 texture=0 shape=0 objtype=0 ammo=0 movetype=0 sound=0 missiondata=0"
    assert "content_error" not in buckets(text)
    assert "content_summary_missing" not in buckets(text)


def test_broken_fixture_exact_counts():
    text = "[CONTENT_SUMMARY] mission=broken total=3 texture=1 shape=1 objtype=0 ammo=0 movetype=0 sound=0 missiondata=1"
    summary = parse_log(text)
    assert summary.content_errors == 3
    assert summary.content_counts == dict(texture=1, shape=1, objtype=0, ammo=0, movetype=0, sound=0, missiondata=1)
    assert "content_error" in buckets(text)
    assert "content_error" not in buckets(text, 3)
    assert "content_error" in buckets(text, 2)


def test_missing_or_malformed_summary_fails():
    assert "content_summary_missing" in buckets("")
    assert "content_summary_missing" in buckets("[CONTENT_SUMMARY] total=oops")
    assert "content_summary_missing" in buckets("[CONTENT_SUMMARY] total=0")
    assert "content_summary_missing" in buckets("[CONTENT_SUMMARY] total=0 texture=1 shape=0 objtype=0 ammo=0 movetype=0 sound=0 missiondata=0")


def test_smoke_report_retains_content_counts():
    summary = parse_log("[CONTENT_SUMMARY] mission=broken total=1 texture=0 shape=1 objtype=0 ammo=0 movetype=0 sound=0 missiondata=0")
    rows = [report.Row("broken", Verdict(False, ["content_error"]), summary)]
    result = report.render_json(rows, tier="adhoc", profile="stock", timestamp="fixture")
    assert result["rows"][0]["content_errors"] == 1
    assert result["rows"][0]["content_counts"]["shape"] == 1
    assert "broken=1" in report.render_markdown(rows, tier="adhoc", profile="stock", timestamp="fixture")
