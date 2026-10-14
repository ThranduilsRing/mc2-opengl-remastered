import copy
import json

import pytest

from tools.vfx_oracle.compare_combat_traces import (
    InvalidTrace, compare, main, read_trace,
)


def fixture():
    events = []
    for kind, slot in (("bolt", 3), ("carnage", 4)):
        for seq, event in enumerate(("birth", "position", "impact", "damage", "position", "retire")):
            data = dict(kind=kind, slot=slot, generation=1, seq=seq, tick=seq,
                        event=event, reason="hit" if event == "impact" else "",
                        effect_id=8, type=12)
            if event != "birth":
                data["position_bits"] = [0, 1, 2]
            if event == "damage":
                data.update(target=9, damage_bits=0x3F800000, location=0,
                            target_position_bits=[3, 4, 5])
            events.append(dict(tag="VFX_COMBAT", v=1, session_id="a", pid=1, data=data))
    return sorted(events, key=lambda e: e["data"]["tick"])


def read(rows):
    return read_trace(json.dumps(row) for row in rows)


def test_equal_ignores_session_wall_clock_and_deadline_metadata():
    a = fixture()
    b = copy.deepcopy(a)
    for row in b:
        row.update(session_id="b", pid=123, ts_ms=99, written_epoch=1234, frame=87)
        row["data"].update(deadline_tick=100, remaining_ticks=33, deadline_source="policy")
    assert compare(read(a), read(b))["status"] == "equal"


def test_deadline_event_does_not_shift_observed_order():
    a = fixture()
    b = copy.deepcopy(a)
    deadline = copy.deepcopy(b[0])
    deadline["data"].update(event="deadline", tick=0, seq=1, deadline_tick=20)
    b.insert(2, deadline)
    for row in b:
        if row["data"]["slot"] == 3 and row["data"]["event"] not in {"birth", "deadline"}:
            row["data"]["seq"] += 1
    assert compare(read(a), read(b))["status"] == "equal"


@pytest.mark.parametrize("event,field,value,cause", [
    ("damage", "damage_bits", 0x40000000, "damage"),
    ("position", "position_bits", [0, 1, 3], "position"),
    ("retire", "tick", 6, "retire"),
])
def test_negative_controls(event, field, value, cause):
    a = fixture()
    b = copy.deepcopy(a)
    changed = next(r["data"] for r in b if r["data"]["event"] == event)
    changed[field] = value
    b.sort(key=lambda r: r["data"]["tick"])
    result = compare(read(a), read(b))
    assert result["status"] == "diverged"
    assert result["first_observed_divergence"]["class"] == cause
    assert result["first_observed_divergence"]["tick"] == (1 if event == "position" else 3 if event == "damage" else 5)


def test_earlier_damage_cannot_be_hidden_by_later_retirement_change():
    a = fixture()
    b = copy.deepcopy(a)
    next(r["data"] for r in b if r["data"]["event"] == "damage")["location"] = 1
    b[-1]["data"]["tick"] = 8
    result = compare(read(a), read(b))
    assert result["first_observed_divergence"]["class"] == "damage"
    assert result["matching_positions"] == 2


def test_empty_combat_is_not_a_pass():
    with pytest.raises(InvalidTrace, match="no VFX_COMBAT"):
        read([dict(tag="CONFIG")])
    events = read(fixture()[:2])
    result = compare(events, events)
    assert result["status"] == "insufficient_coverage"
    assert "bolt:damage" in result["missing_coverage"]["left"]


def test_capture_end_is_not_accepted_as_shifted_retirement():
    events = read(fixture())
    result = compare(events, events[:-1], required=())
    assert result["first_observed_divergence"]["class"] == "capture_end"


def test_earlier_extra_retirement_precedes_the_other_runs_later_damage():
    events = read(fixture())
    index = next(i for i, e in enumerate(events) if e["event"] == "damage")
    changed = copy.deepcopy(events)
    changed.insert(index, dict(kind="bolt", slot=3, generation=1, tick=2,
                               event="retire", reason="deadline"))
    result = compare(events, changed)
    assert result["first_observed_divergence"]["class"] == "retire"
    assert result["first_observed_divergence"]["tick"] == 2


def test_pool_generations_are_distinct():
    a = fixture()
    b = copy.deepcopy(a)
    for row in b:
        row["data"]["generation"] += 1
        row["data"]["tick"] += 10
    events = read(a + b)
    assert compare(events, events)["status"] == "equal"
    b[0]["data"]["generation"] = 1
    with pytest.raises(InvalidTrace, match="reused generation"):
        read(a + b)


@pytest.mark.parametrize("mutation,match", [
    (lambda a: a[2]["data"].update(seq=9), "sequence gap"),
    (lambda a: a[2].update(session_id="other"), "multiple engine sessions"),
    (lambda a: a[4]["data"].update(tick=0), "clock reset"),
    (lambda a: a[2]["data"].update(position_bits=[float("nan"), 0, 0]), "malformed position_bits"),
    (lambda a: a[6]["data"].pop("damage_bits"), "incomplete damage"),
])
def test_invalid_traces_fail_closed(mutation, match):
    a = fixture()
    mutation(a)
    with pytest.raises(InvalidTrace, match=match):
        read(a)


def test_truncated_or_post_retirement_events_rejected():
    with pytest.raises(InvalidTrace, match="invalid JSON"):
        read_trace(['{"tag":'])
    rows = fixture()
    extra = copy.deepcopy(rows[-1])
    extra["data"].update(event="position", seq=6, tick=6)
    with pytest.raises(InvalidTrace, match="outside a live generation"):
        read(rows + [extra])


def test_cli_exit_codes_without_temp_files(monkeypatch, capsys):
    import io
    from pathlib import Path
    a = fixture()
    b = copy.deepcopy(a)
    b[-1]["data"]["tick"] = 6
    payloads = {"a": a, "b": b, "empty": []}
    monkeypatch.setattr(Path, "open", lambda path, **kw: io.StringIO(
        "\n".join(json.dumps(r) for r in payloads[str(path)])))
    assert main(["a", "a"]) == 0
    assert main(["a", "b"]) == 1
    assert main(["a", "b", "--expect", "retire"]) == 0
    assert main(["a", "empty", "--expect", "retire"]) == 2
    assert '"invalid_trace"' in capsys.readouterr().out
