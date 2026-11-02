import sys
from pathlib import Path

D = Path(__file__).parent / "data"
sys.path.insert(0, str(Path(__file__).parent.parent))
import campaign_liveness as cl  # noqa: E402
import campaign_roster as cr  # noqa: E402

FIT = (D / "mc2_01.fit").read_text(errors="replace")
TYPES = cr.load_object_types([D / "buildings.csv"])
LOG = cl.parse_log((D / "mover_v2_sample.txt").read_text())


def test_fit_parts_and_types():
    parts = cr.parse_fit_parts(FIT)
    assert len(parts) == 29
    assert parts[0]["name"] == "Werewolf" and parts[0]["player"] and parts[0]["team"] == 0
    assert TYPES["werewolf"][0] == "MECH"


def test_expected_roster_mechs_only():
    r = cr.expected_roster(FIT, TYPES)
    kinds = {m["kind"] for m in r["mechs"]}
    assert kinds == {"mech"} and r["mechs"]
    with_veh = cr.expected_roster(FIT, TYPES, movers=True)
    assert len(with_veh["mechs"]) >= len(r["mechs"])


def test_late_flag_from_fit():
    fit = FIT.replace("l Active = 1", "l Active = 0", 1)
    r = cr.expected_roster(fit, TYPES)
    assert any(m["late"] for m in r["mechs"])


def test_compare_never_seen_and_passed():
    r = cr.expected_roster(FIT, TYPES)
    res = cr.compare(r, LOG[0])
    c = res["counts"]
    assert c["passed"] + c["failed"] + c["never_seen"] == len(r["mechs"])
    assert c["passed"] > 0 and c["failed"] == 0


def test_compare_with_captures_failed_and_missing():
    r = cr.expected_roster(FIT, TYPES)
    seen = cr.compare(r, LOG[0])["rows"]
    ids = [x["id"] for x in seen if x["id"] is not None]
    caps = [{"id": ids[0], "passed": False}] + [{"id": i, "passed": True} for i in ids[2:]]
    res = cr.compare(r, LOG[0], caps)
    states = {x["id"]: x["state"] for x in res["rows"] if x["id"] is not None}
    assert states[ids[0]] == "failed"      # capture failed pixel check
    assert states[ids[1]] == "failed"      # no capture at all
    assert states[ids[2]] == "passed"


def test_abl_triggers():
    t = cr.abl_late_triggers("integer ReinforceGo; // x\nboolean Foo;\nreal spawnTimer;\n")
    assert t == ["ReinforceGo", "spawnTimer"]
