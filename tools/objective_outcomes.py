"""Compare extracted objective observations with independently authored expectations."""
import argparse
import json
import math
from pathlib import Path
import re
import sys

PREFIX = "OBJECTIVES/1 "
MAX_RECORD_BYTES = 16384
KINDS = {"resolve", "reset", "activate", "dispatch", "sink", "complete"}
SEMANTIC = {"id", "kind", "cause", "data"}
FIELDS = SEMANTIC | {"mission", "seq", "frame", "time"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def decode(text):
    return json.loads(text, object_pairs_hook=unique_object,
                      parse_constant=lambda value: require(False, f"nonfinite JSON: {value}"))


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def identifier(value):
    return isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.:-]{1,96}", value)


def semantic(event):
    require(isinstance(event, dict) and SEMANTIC <= event.keys(), "missing semantic fields")
    require(identifier(event["id"]), "invalid occurrence ID")
    require(isinstance(event["kind"], str) and event["kind"] in KINDS, "unknown event kind")
    require(event["cause"] is None or identifier(event["cause"]), "invalid cause ID")
    require(isinstance(event["data"], dict), "data must be an object")
    return {key: event[key] for key in SEMANTIC}


def compare(lines, expected):
    """Raise ValueError on the first defect; a pass establishes synthetic agreement only."""
    require(isinstance(expected, dict) and set(expected) ==
            {"version", "mission", "events", "before", "timing"}, "invalid expectation fields")
    require(type(expected["version"]) is int and expected["version"] == 1, "wrong version")
    mission = expected["mission"]
    require(isinstance(mission, str) and re.fullmatch(r"[0-9a-f]{64}", mission), "invalid mission hash")
    require(isinstance(expected["events"], list) and expected["events"], "empty expectations")
    wanted = {}
    for event in expected["events"]:
        value = semantic(event)
        require(set(event) == SEMANTIC and event["id"] not in wanted, "invalid/duplicate expected event")
        wanted[event["id"]] = canonical(value)
    require(isinstance(expected["before"], list) and isinstance(expected["timing"], dict),
            "explicit before/timing assumptions required")
    seen = {}
    dispatched = set()
    previous_frame, previous_time = -1, -1
    completed = False
    for seq, line in enumerate(lines, 1):
        require(not completed, "unexpected trailing event")
        require(isinstance(line, str) and len(line.encode("utf-8")) <= MAX_RECORD_BYTES,
                "oversized observation")
        require(line.startswith(PREFIX), "invalid observation prefix")
        event = decode(line[len(PREFIX):])
        value = semantic(event)
        require(set(event) == FIELDS, "invalid observation fields")
        require(event["mission"] == mission, "wrong mission hash")
        require(type(event["seq"]) is int and event["seq"] == seq, "noncontiguous sequence")
        require(type(event["frame"]) is int and event["frame"] >= 0 and event["frame"] >= previous_frame and
                number(event["time"]) and event["time"] >= 0 and event["time"] >= previous_time,
                "invalid frame/mission time")
        previous_frame, previous_time = event["frame"], event["time"]
        event_id, kind, cause = event["id"], event["kind"], event["cause"]
        require(event_id not in seen, "duplicate occurrence ID")
        require(event_id in wanted and canonical(value) == wanted[event_id],
                f"unexpected event/content: {event_id}")
        require(cause is None or cause in seen, "cause must precede event")
        if kind in {"dispatch", "sink"}:
            parent = "resolve" if kind == "dispatch" else "dispatch"
            require(cause in seen and seen[cause][1]["kind"] == parent, "invalid correlation")
        if kind == "dispatch":
            data = event["data"]
            require({"team", "objective", "list", "entry", "species", "payload", "result"} == data.keys(),
                    "invalid dispatch fields")
            require(all(type(data[key]) is int and data[key] >= 0 for key in ("team", "objective", "entry"))
                    and data["team"] < 8 and data["list"] in {"success", "failure"}
                    and isinstance(data["species"], str), "invalid dispatch identity")
            resolution = seen[cause][1]["data"]
            require(all(data[key] == resolution.get(key) for key in ("team", "objective")),
                    "dispatch targets a different resolution")
            identity = (cause, data["team"], data["objective"], data["list"], data["entry"])
            require(identity not in dispatched, "extra dispatch for resolution/entry")
            dispatched.add(identity)
        if kind == "complete":
            require(event["data"].keys() == {"count", "result"} and
                    type(event["data"]["count"]) is int and event["data"]["count"] == seq - 1,
                    "invalid completion count")
            completed = True
        seen[event_id] = (seq, event)
    require(completed, "missing completion")
    require(seen.keys() == wanted.keys(), "missing expected events")
    for edge in expected["before"]:
        require(isinstance(edge, list) and len(edge) == 2 and
                all(isinstance(key, str) and key in seen for key in edge), "invalid causal edge")
        require(seen[edge[0]][0] < seen[edge[1]][0], "causal order violated")
    for event_id, bounds in expected["timing"].items():
        require(event_id in seen and isinstance(bounds, list) and len(bounds) == 2 and
                all(number(bound) and bound >= 0 for bound in bounds) and bounds[0] <= bounds[1],
                "invalid absolute-time bounds")
        require(bounds[0] <= seen[event_id][1]["time"] <= bounds[1], "absolute-time assumption violated")
    return len(seen)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("expected", type=Path)
    parser.add_argument("--mission", help="case in an authored fixture family")
    args = parser.parse_args(argv)
    try:
        expected = decode(args.expected.read_text(encoding="utf-8"))
        with args.trace.open(encoding="utf-8", newline="") as stream:
            lines = [line.removesuffix("\n") for line in stream]
        if args.mission:
            count = compare_family(lines, expected, args.mission, args.expected.parent)
        else:
            count = compare(lines, expected)
    except (OSError, ValueError, TypeError, KeyError, OverflowError, RecursionError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {count} observations match (no runtime outcome claim)")
    return 0


def matches(actual, pattern):
    """Only declared volatile clocks, traversal identities and mounted paths vary."""
    if isinstance(pattern, dict):
        if set(pattern) == {"$integer"}:
            return type(actual) is int and pattern["$integer"][0] <= actual <= pattern["$integer"][1]
        if set(pattern) == {"$oneOf"}:
            return any(type(actual) is type(item) and actual == item for item in pattern["$oneOf"])
        if set(pattern) == {"$path"}:
            return isinstance(actual, str) and actual.replace("\\", "/").lower().endswith(pattern["$path"])
        return isinstance(actual, dict) and actual.keys() == pattern.keys() and all(
            matches(actual[key], value) for key, value in pattern.items())
    return type(actual) is type(pattern) and actual == pattern


def compare_family(lines, family, case, directory):
    import hashlib
    import struct
    require(family.get("version") == 2 and case in family["cases"], "unknown fixture case")
    expected = family["cases"][case]
    digest = hashlib.sha256(b"MC2-OBJECTIVES/1\0")
    for extension in ("fit", "pak"):
        data = (directory / "mod/data/missions" / f"{case}.{extension}").read_bytes()
        require(hashlib.sha256(data).hexdigest() == expected["inputs"][extension], "fixture input changed")
        digest.update(struct.pack("<Q", len(data)))
        digest.update(data)
    require(digest.hexdigest() == expected["mission"], "fixture fingerprint changed")
    for name, digest in family["support"].items():
        require(hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest, "fixture support changed")
    observed = []
    for line in lines:
        require(isinstance(line, str) and line.startswith(PREFIX) and
                len(line.encode("utf-8")) <= MAX_RECORD_BYTES, "invalid observation")
        observed.append(decode(line[len(PREFIX):]))
    require(len(observed) == len(expected["events"]), "missing/extra fixture observations")
    bound, used, materialized = {}, set(), []
    for pattern in expected["events"]:
        cause = None if pattern["cause"] is None else bound.get(pattern["cause"])
        require(pattern["cause"] is None or cause is not None, "oracle cause must precede dependent")
        candidates = [event for event in observed if event["id"] not in used and
                      event["kind"] == pattern["kind"] and event["cause"] == cause and
                      matches(event["data"], pattern["data"])]
        require(len(candidates) == 1, f"missing/ambiguous fixture event: {pattern['id']}")
        event = candidates[0]
        bound[pattern["id"]] = event["id"]
        used.add(event["id"])
        materialized.append(semantic(event))
    oracle = {"version": 1, "mission": expected["mission"], "events": materialized,
              "before": [[bound[a], bound[b]] for a, b in expected["before"]],
              "timing": {bound[key]: value for key, value in expected["timing"].items()}}
    count = compare(lines, oracle)
    by_id = {event["id"]: event for event in observed}
    for name, delta in expected.get("cbills", {}).items():
        data = by_id[bound[name]]["data"]
        require(data["new"] - data["old"] == delta, "wrong objective C-Bill delta")
    for earlier, later in zip(expected.get("cbills", {}), list(expected.get("cbills", {}))[1:]):
        require(by_id[bound[earlier]]["data"]["new"] == by_id[bound[later]]["data"]["old"],
                "discontinuous C-Bill settlement")
    for earlier, later in expected.get("later_traversal", []):
        require(by_id[bound[earlier]]["data"]["traversal"] < by_id[bound[later]]["data"]["traversal"],
                "activation must precede resolution in a later traversal")
    return count


if __name__ == "__main__":
    sys.exit(main())
