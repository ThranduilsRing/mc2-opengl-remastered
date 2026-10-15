"""Synthetic contract checks; these traces are not engine evidence."""
from copy import deepcopy
import io
import json

import pytest

from objective_outcomes import MAX_RECORD_BYTES, PREFIX, Path, compare, main

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
