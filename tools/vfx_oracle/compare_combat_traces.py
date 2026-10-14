"""Find the first observed G3 trace divergence; this is not a whole-sim proof."""
from __future__ import annotations

import argparse
from collections import Counter
from itertools import zip_longest
import json
from pathlib import Path
from typing import Iterable


TAG = "VFX_COMBAT"
EVENTS = {"birth", "restore", "position", "impact", "damage", "retire",
          "collision_armed", "static_collision_pass", "deadline"}
# Newly added policy metadata is never evidence of a gameplay divergence.
METADATA = {"seq", "deadline", "deadline_tick", "remaining_ticks", "deadline_source",
            "deadline_metadata"}
DEFAULT_COVERAGE = ("bolt:impact:hit", "bolt:damage", "bolt:position", "bolt:retire",
                    "carnage:damage", "carnage:position", "carnage:retire")


class InvalidTrace(ValueError):
    pass


def _integer(value, minimum=0, maximum=None):
    return (type(value) is int and value >= minimum
            and (maximum is None or value <= maximum))


def read_trace(lines: Iterable[str], *, exact_payload: bool = False) -> list[dict]:
    """Validate one engine session; ignore unrelated diagnostic events."""
    events = []
    session = None
    sequences = {}
    generations = {}
    retired = set()
    last_tick = -1
    for number, line in enumerate(lines, 1):
        try:
            row = json.loads(line)
        except (ValueError, TypeError) as exc:
            raise InvalidTrace(f"line {number}: invalid JSON") from exc
        if not isinstance(row, dict):
            raise InvalidTrace(f"line {number}: expected event object")
        if row.get("tag") != TAG:
            continue
        if row.get("v") != 1:
            raise InvalidTrace(f"line {number}: unsupported trace version")
        identity = (row.get("session_id"), row.get("pid"))
        if not identity[0] or not _integer(identity[1], 1):
            raise InvalidTrace(f"line {number}: missing session identity")
        if session is None:
            session = identity
        if session != identity:
            raise InvalidTrace(f"line {number}: multiple engine sessions; split captures")
        data = row.get("data")
        if not isinstance(data, dict):
            raise InvalidTrace(f"line {number}: expected data object")
        if (data.get("kind") not in {"bolt", "carnage"}
                or data.get("event") not in EVENTS
                or not _integer(data.get("slot"))
                or not _integer(data.get("generation"), 1)
                or not _integer(data.get("seq"))
                or not _integer(data.get("tick"))):
            raise InvalidTrace(f"line {number}: malformed combat identity/event")
        if data["tick"] < last_tick:
            raise InvalidTrace(f"line {number}: clock reset; split at load")
        last_tick = data["tick"]
        slot = (data["kind"], data["slot"])
        key = (*slot, data["generation"])
        if data["event"] == "birth":
            if data["generation"] <= generations.get(slot, 0):
                raise InvalidTrace(f"line {number}: reused generation")
            old_key = (*slot, generations.get(slot, 0))
            if slot in generations and old_key not in retired:
                raise InvalidTrace(f"line {number}: pool reuse without retirement")
            generations[slot] = data["generation"]
            sequences[key] = -1
        if key not in sequences or key in retired:
            raise InvalidTrace(f"line {number}: event outside a live generation")
        if data["seq"] != sequences[key] + 1:
            raise InvalidTrace(f"line {number}: event sequence gap")
        sequences[key] = data["seq"]
        if data["event"] == "retire":
            retired.add(key)
        for field in ("position_bits", "flight_bits", "target_position_bits"):
            if field in data and (not isinstance(data[field], list) or len(data[field]) != 3
                                  or not all(_integer(v, 0, 0xFFFFFFFF) for v in data[field])):
                raise InvalidTrace(f"line {number}: malformed {field}")
        if data["event"] == "position" and "position_bits" not in data:
            raise InvalidTrace(f"line {number}: missing position")
        if data["event"] == "damage" and (
                not _integer(data.get("target"))
                or not _integer(data.get("damage_bits"), 0, 0xFFFFFFFF)
                or "target_position_bits" not in data
                or not _integer(data.get("location"), -2147483648)):
            raise InvalidTrace(f"line {number}: incomplete damage submission")
        if exact_payload or data["event"] != "deadline":
            events.append(dict(data) if exact_payload else
                          {k: v for k, v in data.items() if k not in METADATA})
    if not events:
        raise InvalidTrace("no VFX_COMBAT events")
    return events


def coverage(events: list[dict]) -> dict[str, int]:
    counts = Counter()
    for event in events:
        base = f"{event['kind']}:{event['event']}"
        counts[base] += 1
        if event.get("reason"):
            counts[f"{base}:{event['reason']}"] += 1
    return dict(sorted(counts.items()))


def compare(left: list[dict], right: list[dict], required=DEFAULT_COVERAGE) -> dict:
    counts = {"left": coverage(left), "right": coverage(right)}
    missing = {side: [path for path in required if not found.get(path)]
               for side, found in counts.items()}
    result = {"coverage": counts, "missing_coverage": missing,
              "matching_events": 0, "matching_positions": 0, "matching_damage": 0}
    for index, (a, b) in enumerate(zip_longest(left, right)):
        if a != b:
            candidates = [e for e in (a, b) if e is not None]
            # An equal position followed by an extra retirement is a retirement divergence;
            # two position records differing in bits or tick are a position divergence.
            first_tick = min(e["tick"] for e in candidates)
            earliest = [e for e in candidates if e["tick"] == first_tick]
            if not a or not b:
                cause = "capture_end"
            elif a["event"] == b["event"]:
                cause = a["event"]
            else:
                cause = next((name for name in ("damage", "retire", "position")
                              if any(e["event"] == name for e in earliest)), "lifecycle")
            result.update(status="diverged", first_observed_divergence={
                "index": index, "tick": first_tick,
                "class": cause, "left": a, "right": b})
            break
        result["matching_events"] += 1
        result["matching_positions"] += a["event"] == "position"
        result["matching_damage"] += a["event"] == "damage"
    else:
        result.update(status="equal", first_observed_divergence=None)
    if any(missing.values()):
        result["comparison_status"] = result["status"]
        result["status"] = "insufficient_coverage"
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--require", action="append", default=[], metavar="KIND:EVENT[:REASON]",
                        help="additional nonzero path coverage required in BOTH traces")
    parser.add_argument("--expect", choices=("equal", "retire"), default="equal")
    parser.add_argument("--exact-payload", action="store_true",
                        help="also retain sequence and policy fields and deadline events")
    args = parser.parse_args(argv)
    try:
        with args.left.open(encoding="utf-8") as a, args.right.open(encoding="utf-8") as b:
            result = compare(read_trace(a, exact_payload=args.exact_payload),
                             read_trace(b, exact_payload=args.exact_payload),
                             (*DEFAULT_COVERAGE, *args.require))
    except (OSError, InvalidTrace) as exc:
        print(json.dumps({"status": "invalid_trace", "error": str(exc)}))
        return 2
    print(json.dumps(result, indent=2))
    if args.expect == "equal":
        return 0 if result["status"] == "equal" else 1
    return 0 if (result["status"] == "diverged"
                 and result["first_observed_divergence"]["class"] == "retire") else 1


if __name__ == "__main__":
    raise SystemExit(main())
