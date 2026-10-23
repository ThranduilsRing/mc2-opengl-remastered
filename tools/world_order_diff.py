#!/usr/bin/env python3
"""Compare WORLD_ORDER v1 logs, naming the first tick, record and field that differs.

Default: all complete ticks must agree, including length. --common-prefix compares
only the complete ticks both wall-clock harness runs reached. Empty, malformed,
missing, repeated or internally truncated ticks always fail.
"""
from __future__ import annotations

import argparse
from itertools import zip_longest
from pathlib import Path
import re

PREFIX = "[WORLD_ORDER v1] "
RECORD = re.compile(r"\[WORLD_ORDER v1\] tick=(\d+) seq=(\d+) ([^\r\n\[]*)")


def load_ticks(path):
    """Yield one validated tick at a time; memory is bounded by transcript bytes/tick."""
    current, records, previous = None, [], None
    with open(path, encoding="utf-8", errors="replace") as source:
        for line in source:
            matches = list(RECORD.finditer(line))
            if line.count(PREFIX) != len(matches):
                raise ValueError(f"{path}: malformed WORLD_ORDER record")
            for match in matches:
                tick, seq, body = int(match[1]), int(match[2]), match[3].rstrip()
                if current is None:
                    if seq != 0 or body != "begin":
                        raise ValueError(f"{path}: tick {tick} missing begin")
                    if previous is not None and tick != previous + 1:
                        raise ValueError(f"{path}: missing/repeated tick {previous} -> {tick}")
                    current = tick
                if tick != current or seq != len(records):
                    raise ValueError(f"{path}: tick {tick} missing/repeated record {seq}, expected {len(records)}")
                records.append(body)
                if body == "end":
                    if not any(r.startswith("snapshot=gom_begin ") for r in records):
                        raise ValueError(f"{path}: tick {tick} missing object-manager observation")
                    yield tick, records
                    previous, current, records = tick, None, []
    if current is not None:
        raise ValueError(f"{path}: truncated tick {current}")


def first_difference(a, b):
    """Find a readable token difference, including view slots and cursor values."""
    for left, right in zip_longest(a.split(), b.split(), fillvalue="<missing>"):
        if left != right:
            if left.startswith("slots=") and right.startswith("slots="):
                for slot, (x, y) in enumerate(zip_longest(left[6:].split(','), right[6:].split(','), fillvalue="<missing>")):
                    if x != y:
                        return f"slot {slot}: {x} != {y}"
            return f"{left} != {right}"
    return "record differs"


def compare(path_a, path_b, *, common_prefix=False, min_ticks=1):
    compared, bytes_seen = 0, 0
    try:
        for a, b in zip_longest(load_ticks(path_a), load_ticks(path_b)):
            if a is None or b is None:
                if common_prefix:
                    # Consume and validate the longer tail; a broken tick is never accepted.
                    continue
                return False, f"DIVERGED after {compared} ticks: one transcript ended"
            if a[0] != b[0]:
                return False, f"DIVERGED tick: {a[0]} != {b[0]}"
            for seq, (left, right) in enumerate(zip_longest(a[1], b[1], fillvalue="<missing>")):
                if left != right:
                    return False, (f"DIVERGED tick {a[0]} record {seq}: {first_difference(left, right)}\n"
                                   f"  a: {left}\n  b: {right}")
                bytes_seen += len(f"{PREFIX}tick={a[0]} seq={seq} {left}\n".encode())
            compared += 1
    except (OSError, ValueError) as error:
        return False, str(error)
    if compared < min_ticks:
        return False, f"only {compared} complete common ticks (need {min_ticks})"
    return True, f"identical over {compared} ticks; {bytes_seen / compared:.1f} transcript bytes/tick"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_a", type=Path)
    parser.add_argument("log_b", type=Path)
    parser.add_argument("--common-prefix", action="store_true")
    parser.add_argument("--min-ticks", type=int, default=1)
    args = parser.parse_args(argv)
    ok, message = compare(args.log_a, args.log_b, common_prefix=args.common_prefix, min_ticks=max(1, args.min_ticks))
    print(f"world_order_diff: {'PASS' if ok else 'FAIL'} {message}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
