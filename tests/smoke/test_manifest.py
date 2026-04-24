from pathlib import Path
from scripts.smoke_lib.manifest import parse_manifest, Entry

SAMPLE = """
# comment
tier1 mc2_01 reason="baseline grass/desert combat"
tier1 mc2_17 duration=180 heartbeat_timeout_load=120 reason="large map"
tier2 mc2_02
skip ai_glenn reason="dev leftover"
"""

def test_parse_collects_tiered_entries(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text(SAMPLE)
    entries = parse_manifest(p)
    assert [e.stem for e in entries if e.tier == "tier1"] == ["mc2_01", "mc2_17"]
    assert [e.stem for e in entries if e.tier == "tier2"] == ["mc2_02"]
    assert [e.stem for e in entries if e.tier == "skip"] == ["ai_glenn"]

def test_parse_handles_overrides(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text(SAMPLE)
    entries = parse_manifest(p)
    e17 = next(e for e in entries if e.stem == "mc2_17")
    assert e17.duration == 180
    assert e17.heartbeat_timeout_load == 120
    assert e17.reason == "large map"

def test_parse_ignores_comments_and_blanks(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text("# a\n\n  # b\n\ntier1 mc2_01\n")
    entries = parse_manifest(p)
    assert len(entries) == 1

def test_parse_skip_section_excluded_from_other_tiers(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text("tier1 mc2_01\nskip mc2_01 reason=\"broken\"\n")
    entries = parse_manifest(p)
    # skip wins: one tier1 entry, one skip entry, runner filters skip out of tier runs.
    tiers = {e.tier for e in entries if e.stem == "mc2_01"}
    assert tiers == {"tier1", "skip"}
