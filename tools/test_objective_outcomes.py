"""Synthetic contract checks; these traces are not engine evidence."""
from copy import deepcopy
import io
import json

import pytest

from objective_outcomes import MAX_RECORD_BYTES, PREFIX, Path, compare, compare_family, main

MISSION = "a" * 64

# The trace and semantic oracle are separately written to expose omissions in either.
TRACE = '''OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":1,"id":"r1","frame":40,"time":10,"kind":"resolve","cause":null,"data":{"team":0,"objective":2,"old":"unresolved","new":"success"}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":2,"id":"a1","frame":40,"time":10,"kind":"dispatch","cause":"r1","data":{"team":0,"objective":2,"list":"success","entry":0,"species":"DisplayTextMessage","payload":"ready","result":true}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":3,"id":"s1","frame":40,"time":10,"kind":"sink","cause":"a1","data":{"sink":"chat","result":"accepted"}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":4,"id":"z1","frame":40,"time":10,"kind":"reset","cause":"r1","data":{"team":0,"objective":2,"old":"success","new":"unresolved"}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":5,"id":"r2","frame":40,"time":10,"kind":"resolve","cause":"z1","data":{"team":0,"objective":2,"old":"unresolved","new":"success"}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":6,"id":"a2","frame":40,"time":10,"kind":"dispatch","cause":"r2","data":{"team":0,"objective":2,"list":"success","entry":0,"species":"DisplayTextMessage","payload":"ready","result":true}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":7,"id":"s2","frame":40,"time":10,"kind":"sink","cause":"a2","data":{"sink":"chat","result":"accepted"}}
OBJECTIVES/1 {"mission":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","seq":8,"id":"end","frame":41,"time":10.25,"kind":"complete","cause":null,"data":{"count":7,"result":"horizon"}}'''


def expectation():
    return {
        "version": 1, "mission": MISSION,
        "events": [
            {"id": "r1", "kind": "resolve", "cause": None,
             "data": {"team": 0, "objective": 2, "old": "unresolved", "new": "success"}},
            {"id": "a1", "kind": "dispatch", "cause": "r1",
             "data": {"team": 0, "objective": 2, "list": "success", "entry": 0,
                      "species": "DisplayTextMessage", "payload": "ready", "result": True}},
            {"id": "s1", "kind": "sink", "cause": "a1",
             "data": {"sink": "chat", "result": "accepted"}},
            {"id": "z1", "kind": "reset", "cause": "r1",
             "data": {"team": 0, "objective": 2, "old": "success", "new": "unresolved"}},
            {"id": "r2", "kind": "resolve", "cause": "z1",
             "data": {"team": 0, "objective": 2, "old": "unresolved", "new": "success"}},
            {"id": "a2", "kind": "dispatch", "cause": "r2",
             "data": {"team": 0, "objective": 2, "list": "success", "entry": 0,
                      "species": "DisplayTextMessage", "payload": "ready", "result": True}},
            {"id": "s2", "kind": "sink", "cause": "a2",
             "data": {"sink": "chat", "result": "accepted"}},
            {"id": "end", "kind": "complete", "cause": None,
             "data": {"count": 7, "result": "horizon"}},
        ],
        "before": [["s1", "z1"], ["s2", "end"]],
        "timing": {"r1": [10, 11], "end": [10, 12]},
    }


def records():
    return [json.loads(line[len(PREFIX):]) for line in TRACE.splitlines()]


def lines(events):
    return [PREFIX + json.dumps(event) for event in events]


def test_reset_replay_with_same_frame_and_entry_is_allowed():
    assert compare(TRACE.splitlines(), expectation()) == 8


def test_independent_events_may_reorder_without_invented_frames():
    expected = expectation()
    expected["events"].reverse()
    expected["before"] = [["s2", "end"]]
    events = records()
    events[2], events[3] = events[3], events[2]
    for seq, event in enumerate(events, 1):
        event["seq"] = seq
        event["frame"] += 100
    assert compare(lines(events), expected) == 8


@pytest.mark.parametrize("defect", ["missing", "duplicate", "reordered", "payload", "mission",
                                   "completion", "trailing", "correlation", "time", "count", "type"])
def test_independently_mutated_trace_is_rejected(defect):
    events = records()
    if defect == "missing":
        del events[2]
    elif defect == "duplicate":
        events.insert(3, deepcopy(events[2]))
    elif defect == "reordered":
        events[1], events[2] = events[2], events[1]
    elif defect == "payload":
        events[1]["data"]["payload"] = "wrong"
    elif defect == "mission":
        events[0]["mission"] = "b" * 64
    elif defect == "completion":
        events.pop()
    elif defect == "trailing":
        events.append(deepcopy(events[1]))
    elif defect == "correlation":
        events[6]["cause"] = "a1"
    elif defect == "time":
        for event in events:
            event["time"] += 20
    elif defect == "count":
        events[-1]["data"]["count"] = 6
    else:
        events[1]["data"]["result"] = 1
    for index, event in enumerate(events, 1):
        event["seq"] = index
    with pytest.raises(ValueError):
        compare(lines(events), expectation())


def test_extra_dispatch_rejected_even_if_oracle_accidentally_lists_it():
    expected = expectation()
    events = records()
    extra = deepcopy(events[1])
    extra["id"] = "extra"
    events.insert(2, extra)
    expected["events"].append({key: extra[key] for key in ("id", "kind", "cause", "data")})
    events[-1]["data"]["count"] = 8
    expected["events"][-2]["data"]["count"] = 8
    for index, event in enumerate(events, 1):
        event["seq"] = index
    with pytest.raises(ValueError, match="extra dispatch"):
        compare(lines(events), expected)


def test_empty_expectations_and_empty_trace_cannot_pass():
    expected = expectation()
    expected["events"] = []
    with pytest.raises(ValueError, match="empty expectations"):
        compare([], expected)
    with pytest.raises(ValueError, match="missing completion"):
        compare([], expectation())


def test_dispatch_cannot_borrow_another_objectives_resolution():
    events = records()
    expected = expectation()
    events[1]["data"]["objective"] = 3
    expected["events"][1]["data"]["objective"] = 3
    with pytest.raises(ValueError, match="different resolution"):
        compare(lines(events), expected)


@pytest.mark.parametrize("bad", ["OBJECTIVES/2 {}", PREFIX + "{", PREFIX + '{"id":1,"id":2}',
                               PREFIX + '{"time":NaN}', PREFIX + "x" * MAX_RECORD_BYTES])
def test_malformed_or_oversized_record_rejects_without_truncation(bad):
    with pytest.raises(ValueError):
        compare([bad], expectation())


def test_semantic_order_and_absolute_time_bounds_are_enforced():
    expected = expectation()
    expected["before"].append(["z1", "s1"])
    with pytest.raises(ValueError, match="causal order"):
        compare(TRACE.splitlines(), expected)


def test_json_escaping_preserves_payload_without_truncation():
    expected = expectation()
    events = records()
    payload = 'quote " newline\nbackslash\\ café'
    events[1]["data"]["payload"] = payload
    expected["events"][1]["data"]["payload"] = payload
    assert compare(lines(events), expected) == 8


@pytest.mark.parametrize("field,value", [("frame", -1), ("frame", True), ("time", -0.5),
                                       ("time", float("inf")), ("seq", True)])
def test_invalid_clock_and_sequence_values_are_rejected(field, value):
    events = records()
    events[0][field] = value
    with pytest.raises(ValueError):
        compare(lines(events), expectation())


@pytest.mark.parametrize("bad", [PREFIX + "{", PREFIX + "x" * MAX_RECORD_BYTES])
def test_cli_emits_one_rejection_and_does_not_resume(monkeypatch, capsys, bad):
    monkeypatch.setattr(Path, "read_text", lambda *args, **kwargs: json.dumps(expectation()))
    monkeypatch.setattr(Path, "open", lambda *args, **kwargs: io.StringIO(bad + "\n" + TRACE))
    assert main(["trace", "expected"]) == 1
    output = capsys.readouterr()
    assert output.out == ""
    assert output.err.count("FAIL:") == 1
    assert len(output.err.splitlines()) == 1


PACKAGE = Path(__file__).resolve().parents[1] / "tests/smoke/objectives_e6e"


def family_trace(loss=False):
    """Synthetic sink receipts are written separately from the JSON oracle."""
    family = json.loads((PACKAGE / "expected.json").read_text())
    case = "e6e_loss" if loss else "e6e_win"
    events = []
    def add(name, kind, data, cause=None, time=2, traversal=20):
        if kind in {"activate", "resolve"} and "objective" in data and "scope" not in data:
            data.update(traversal=traversal, source="team")
        events.append(dict(mission=family["cases"][case]["mission"], id=name, kind=kind,
                           cause=cause, data=data, frame=int(time * 30), time=time))
    def resolved(name, objective, status, time, cause=None, traversal=20):
        add(name, "resolve", dict(team=0, objective=objective, old=0, new=status), cause, time, traversal)
    def action(name, owner, objective, entry, species, payload, time, failure=False):
        add(name, "dispatch", dict(team=0, objective=objective, list="failure" if failure else "success",
                                   entry=entry, species=species, payload=payload, result=1), owner, time)
    def sink(name, owner, target, stage, payload, time, result="accepted"):
        add(name, "sink", dict(sink=target, stage=stage, result=result, payload=payload), owner, time)
    def batch(prefix, objective, time, failed):
        resolved(prefix, objective, 2 if failed else 1, time)
        for index, species, payload in (
            (0, "PlayBIK", "e6e_numbered.bik"), (1, "PlayWAV", "e6e_tone.wav"),
            (2, "DisplayTextMessage", "E6e " + prefix), (3, "DisplayResourceTextMessage", 10000),
            (4, "SetBooleanFlag", {"FlagID": "e6e_" + prefix, "Value": True}),
            (5, "MakeNewTechnologyAvailable", "data/missions/e6e_unlock_" + prefix + ".fit")):
            name = prefix + str(index)
            action(name, prefix, objective, index, species, payload, time, failed)
            if index == 0:
                sink(name+"o", name, "movie", "decoder", "data/movies/e6e_numbered.mp4", time)
                sink(name+"g", name, "movie", "gui", "data/movies/e6e_numbered.bik", time)
                sink(name+"f", name, "movie", "decoded_frame", "data/movies/e6e_numbered.mp4", time)
            elif index == 1:
                sink(name+"s", name, "wav", "resource_channel", dict(requested="./e6e_tone.wav",
                     resource="data/sound/e6e_tone.wav", channel=18, resourceAccepted=True, channelAssigned=True),
                     time, "resource_created_channel_assigned")
            elif index in (2, 3):
                sink(name+"s", name, "chat", "stored", payload if index == 2 else "Grasslands Accent 2 (Forest Floor)", time)
            elif index == 4:
                sink(name+"s", name, "flag", "written", "e6e_"+prefix, time, "true")
            else:
                sink(name+"s", name, "technology", "queued", payload, time)
    if loss:
        resolved("failed", 0, 2, 3)
        action("text", "failed", 0, 0, "DisplayTextMessage", "E6e primary failed", 3, True)
        sink("chat", "text", "chat", "stored", "E6e primary failed", 3)
    else:
        batch("success", 0, 2, False)
        add("activate", "activate", dict(team=0, objective=1, old=0, new=1), time=2)
        resolved("chain", 1, 1, 2.1, "activate", 21)
        action("setter", "chain", 1, 0, "SetBooleanFlag", dict(FlagID="e6e_chain", Value=True), 2.1)
        sink("set", "setter", "flag", "written", "e6e_chain", 2.1, "true")
        batch("failure", 2, 6, True)
        resolved("final", 5, 1, 10)
    terminal, end_time = (1, 12) if loss else (5, 20)
    add("term", "resolve", dict(scope="mission", stage="termination", old=0, new=terminal, started=True), time=end_time-6)
    add("result", "resolve", dict(scope="mission", stage="final", old=-1, new=terminal), time=end_time)
    if not loss:
        add("cash", "resolve", dict(scope="cbills", stage="mission", old=10000, new=10000), time=end_time)
        for name, objective, before, after, cause in (
            ("plus", 0, 10000, 11250, "success"), ("minus", 1, 11250, 11000, "chain"), ("zero", 5, 11000, 11000, "final")):
            add(name, "resolve", dict(scope="cbills", team=0, objective=objective, old=before, new=after), cause, end_time)
        for prefix in ("success", "failure"):
            sink(prefix+"file", prefix+"5", "technology", "purchase_file", "data/missions/e6e_unlock_"+prefix+".fit", end_time, "opened")
            sink(prefix+"available", prefix+"5", "technology", "component", 110, end_time, "available")
    add("done", "complete", dict(count=len(events), result=terminal), time=end_time)
    return family, case, events


@pytest.mark.parametrize("loss", [False, True])
def test_authored_package_accepts_independent_complete_synthetic_receipts(loss, monkeypatch, capsys):
    family, case, events = family_trace(loss)
    for seq, event in enumerate(events, 1):
        event["seq"] = seq
    assert compare_family(lines(events), family, case, PACKAGE) == len(events)
    original = Path.open
    monkeypatch.setattr(Path, "open", lambda path, *args, **kwargs:
                        io.StringIO("\n".join(lines(events))) if path == Path("synthetic") else original(path, *args, **kwargs))
    assert main(["synthetic", str(PACKAGE / "expected.json"), "--mission", case]) == 0
    assert "PASS:" in capsys.readouterr().out


@pytest.mark.parametrize("control", ["never_predicate", "inactive", "omitted_setter", "missing_wav", "missing_video", "missing_purchase", "wrong_reward", "wrong_payload", "duplicate_dispatch", "reordered_actions", "same_traversal"])
def test_package_rejects_false_objective_and_sink_outcomes(control):
    family, case, events = family_trace()
    by_id = {event["id"]: event for event in events}
    if control == "never_predicate":
        events.remove(by_id["final"])
    elif control == "inactive":
        events.remove(by_id["activate"])
    elif control == "omitted_setter":
        events.remove(by_id["setter"])
    elif control == "missing_wav":
        by_id["success1s"]["data"]["result"] = "missing"
        by_id["success1s"]["data"]["payload"]["resourceAccepted"] = False
    elif control == "missing_video":
        events.remove(by_id["success0f"])
    elif control == "missing_purchase":
        by_id["successfile"]["data"]["result"] = "rejected"
    elif control == "wrong_reward":
        by_id["minus"]["data"]["new"] = 11500
    elif control == "same_traversal":
        by_id["chain"]["data"]["traversal"] = 20
    elif control == "wrong_payload":
        by_id["success2"]["data"]["payload"] = "wrong text"
    elif control == "duplicate_dispatch":
        events.insert(1, deepcopy(by_id["success0"]))
    else:
        a, b = events.index(by_id["success2"]), events.index(by_id["success3"])
        events[a:b+2] = events[b:b+2] + events[a:a+2]
    events[-1]["data"]["count"] = len(events)-1
    for seq, event in enumerate(events, 1):
        event["seq"] = seq
    with pytest.raises(ValueError):
        compare_family(lines(events), family, case, PACKAGE)


def test_media_recipe_hashes_and_unavailable_component_control_are_pinned():
    import hashlib
    import wave
    assets = json.loads((PACKAGE / "assets.json").read_text())
    for name, digest in assets["sha256"].items():
        assert hashlib.sha256((PACKAGE / name).read_bytes()).hexdigest() == digest
    with wave.open(str(PACKAGE / "mod/data/sound/e6e_tone.wav")) as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (1, 2, 22050, 11025)
    baseline = (PACKAGE / "mod/data/missions/purchase.fit").read_text()
    assert "b AllComponents = FALSE" in baseline
    assert " = 110\n" not in baseline
    assert baseline.count("l Component") == 254
