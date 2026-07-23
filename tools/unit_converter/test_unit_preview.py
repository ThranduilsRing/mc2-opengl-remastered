# tools/unit_converter/test_unit_preview.py
"""Tests for UNIT-PREVIEW-DUMP-1: the headless per-unit modder preview formatter.
Run: py -3 -m pytest tools/unit_converter

Covers: dump parsing (present/absent section), the clean/dirty verdict, socket +
anim-tag rendering, the converter cross-reference, JSON round-trip, the CLI exit
code (0 clean / 1 findings), and a PYTHONHASHSEED determinism guard.
"""
import json
import os
import subprocess
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))  # so `import unit_converter` works

from unit_converter import unit_preview as UP  # noqa: E402


def _row(**kw):
    base = dict(
        objectId=1, name="Alpha", chassis="Atlas", variant="", is_mech=True,
        sockets=[], missing_socket_count=0,
        anim_tags_available=["Idle", "Move"], anim_tags_missing=[],
        icon_resolved="", finding_count=0, first_finding_code=-1, first_finding_msg="",
    )
    base.update(kw)
    return base


def _dump(rows):
    return {"unitPreview": {"version": 1,
                            "animTagOrder": ["Idle", "Move", "Turn", "FireWeapon"],
                            "rows": rows}}


# --- parsing ---------------------------------------------------------------

def test_absent_section_is_empty():
    assert UP.load_previews({}) == []
    assert UP.load_previews({"unitPreview": {}}) == []


def test_parse_basic_row():
    ps = UP.load_previews(_dump([_row(chassis="Atlas", variant="AS7-D")]))
    assert len(ps) == 1
    p = ps[0]
    assert p.chassis == "Atlas"
    assert p.variant == "AS7-D"
    assert p.is_mech is True
    assert p.label == "Atlas:AS7-D"
    assert p.clean is True


def test_label_falls_back_to_name_then_id():
    p = UP.load_previews(_dump([_row(chassis="", name="Nomad", variant="")]))[0]
    assert p.label == "Nomad"
    p2 = UP.load_previews(_dump([_row(chassis="", name="", objectId=42)]))[0]
    assert p2.label == "obj42"


def test_sockets_parsed_with_present_flag():
    p = UP.load_previews(_dump([_row(sockets=[
        {"name": "Weapon0", "present": True},
        {"name": "Weapon1", "present": False},
    ], missing_socket_count=1)]))[0]
    assert len(p.sockets) == 2
    assert p.sockets[0].present is True
    assert p.sockets[1].present is False
    assert p.missing_socket_count == 1


# --- verdict ---------------------------------------------------------------

def test_clean_when_zero_findings():
    p = UP.load_previews(_dump([_row(finding_count=0)]))[0]
    assert p.clean is True


def test_dirty_when_findings_present():
    p = UP.load_previews(_dump([_row(finding_count=1,
                                     first_finding_code=3,
                                     first_finding_msg="declared socket not found")]))[0]
    assert p.clean is False


# --- report rendering ------------------------------------------------------

def test_report_empty_when_no_data():
    out = UP.format_report([])
    assert "no preview data" in out


def test_report_marks_clean_and_dirty():
    ps = UP.load_previews(_dump([
        _row(chassis="Atlas", finding_count=0),
        _row(chassis="Bad", finding_count=2, first_finding_msg="boom"),
    ]))
    out = UP.format_report(ps)
    assert "CLEAN" in out
    assert "2 FINDING(S)" in out
    assert "boom" in out
    assert "RESULT: 1 unit(s) with findings" in out


def test_report_all_clean_result_line():
    ps = UP.load_previews(_dump([_row(chassis="Atlas"), _row(chassis="Raven")]))
    out = UP.format_report(ps)
    assert "all 2 unit(s) preview CLEAN" in out


def test_report_shows_anim_tags_and_missing():
    ps = UP.load_previews(_dump([_row(anim_tags_available=["Idle"],
                                      anim_tags_missing=["JumpLand"])]))
    out = UP.format_report(ps)
    assert "anim avail : Idle" in out
    assert "anim MISS  : JumpLand" in out


def test_report_non_mech_has_no_anim():
    ps = UP.load_previews(_dump([_row(is_mech=False, chassis="Tank",
                                      anim_tags_available=[])]))
    out = UP.format_report(ps)
    assert "n/a (non-mech" in out


def test_converter_cross_reference():
    ps = UP.load_previews(_dump([_row(chassis="Atlas"), _row(chassis="Ghost")]))
    conv = UP.load_converter_chassis({"units": [{"chassis": "core:atlas"}]})
    out = UP.format_report(ps, conv)
    assert "converter  : known" in out
    assert "NOT in converter report" in out


# --- json ------------------------------------------------------------------

def test_json_round_trip():
    ps = UP.load_previews(_dump([_row(chassis="Atlas", finding_count=1)]))
    j = UP.previews_to_json(ps)
    assert j["unit_count"] == 1
    assert j["dirty_count"] == 1
    assert j["units"][0]["chassis"] == "Atlas"


# --- CLI exit code ---------------------------------------------------------

def _write(tmp_path, name, obj):
    p = tmp_path / name
    p.write_text(json.dumps(obj), encoding="utf-8")
    return str(p)


def test_cli_exit_zero_when_clean(tmp_path, capsys):
    dp = _write(tmp_path, "d.json", _dump([_row(finding_count=0)]))
    rc = UP.main(["--engine-dump", dp])
    assert rc == 0


def test_cli_exit_one_when_findings(tmp_path):
    dp = _write(tmp_path, "d.json", _dump([_row(finding_count=1, first_finding_msg="x")]))
    rc = UP.main(["--engine-dump", dp])
    assert rc == 1


def test_cli_json_flag(tmp_path, capsys):
    dp = _write(tmp_path, "d.json", _dump([_row(finding_count=0)]))
    UP.main(["--engine-dump", dp, "--json"])
    out = capsys.readouterr().out
    parsed = json.loads(out)
    assert parsed["unit_count"] == 1


# --- determinism -----------------------------------------------------------

def test_report_deterministic_across_hashseed(tmp_path):
    dump = _dump([_row(chassis="Atlas"), _row(chassis="Raven", finding_count=1,
                                              first_finding_msg="z")])
    dp = _write(tmp_path, "d.json", dump)
    script = (
        "import json,sys;"
        "sys.path.insert(0, r'%s');"
        "from unit_converter import unit_preview as UP;"
        "d=json.load(open(r'%s'));"
        "sys.stdout.write(UP.format_report(UP.load_previews(d)))"
    ) % (os.path.dirname(_HERE), dp)
    outs = []
    for seed in ("0", "1", "12345"):
        env = dict(os.environ, PYTHONHASHSEED=seed)
        r = subprocess.run([sys.executable, "-c", script], env=env,
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        outs.append(r.stdout)
    assert outs[0] == outs[1] == outs[2]
