import json
from pathlib import Path
from crash_bundle_summary import REQUIRED, summarise


def test_complete_bundle_and_hostile_content_path(tmp_path):
    for name in REQUIRED:
        (tmp_path / name).write_text("present", encoding="utf-8")
    (tmp_path / "profile.json").write_text(json.dumps({"mission_name": "mc2_01", "frame": 120}))
    records = [{"kind": "mod", "id": "base", "priority": -1},
               {"kind": "save_point", "operation": "load", "path": "slot.fit"},
               {"kind": "content", "path": "../../never-open-this", "hash": "0123456789abcdef"}]
    (tmp_path / "content.jsonl").write_text("\n".join(map(json.dumps, records)))
    text, complete = summarise(tmp_path)
    assert complete
    assert "mc2_01 frame=120" in text
    assert "../../never-open-this" in text
    assert "load slot.fit" in text


def test_partial_and_missing_bundle(tmp_path):
    text, complete = summarise(tmp_path)
    assert not complete and "MISSING" in text
    (tmp_path / "content.jsonl").write_text('{}\n{"kind":\n[]\n')
    text, complete = summarise(tmp_path)
    assert not complete and "2 invalid/truncated" in text
