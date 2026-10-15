#!/usr/bin/env python3
"""summarize_sortweapons_probe - read the [B2P3 v1] lines of one or more run logs and state what the sort rule did to Mover::sortWeapons on real data.

Prints calls per shape, calls with equal scores (ties), calls whose order differs between the old rule and the tree's rule, and checks the properties the
rule promises: every call returned rc 0, the new order is the scores in descending order, equal scores keep the listed order, the -1 'none' slot is last.
Exit 1 when a promise is broken or no call was seen.
"""
import re
import sys

LINE = re.compile(r"\[B2P3 v1\] simframe=(\d+) mover=(\d+) shape=(\w+) n=(\d+) rc=(-?\d+) ties=(\d) differ=(\d) scores_x1000=([-\d,]*) old=([-\d,]*) new=([-\d,]*)\s*$")


def main(paths):
    calls = {"all": 0, "list": 0}
    ties = {"all": 0, "list": 0}
    differ = {"all": 0, "list": 0}
    broken = []
    first_tie = None
    malformed = 0
    for path in paths:
        for line in open(path, encoding="utf-8", errors="replace"):
            if "[B2P3 v1]" not in line:
                continue
            m = LINE.search(line)
            try:
                if not m:
                    raise ValueError("does not match the line shape")
                frame, mover, shape, n, rc, tie, diff, scores, old, new = m.groups()
                # an empty field is a call with no scored items: an empty list, not a parse error
                scores = [int(x) for x in scores.split(",")] if scores else []
                new_ids = [int(x) for x in new.split(",")] if new else []
                if shape not in calls:
                    raise ValueError("unknown shape " + shape)
                if len(scores) != len(new_ids):
                    raise ValueError("scores and ids differ in length")
                if old:
                    [int(x) for x in old.split(",")]
            except ValueError as err:
                malformed += 1
                if malformed == 1:
                    print(f"summarize_sortweapons_probe: malformed [B2P3 v1] line in {path} ({err}): {line.strip()[:200]}")
                continue
            # ids as scored: all -> numOther + i (any base); list -> the same plus a trailing -1; recover the listed order from the scores' original order
            calls[shape] += 1
            ties[shape] += int(tie)
            differ[shape] += int(diff)
            if int(tie) and first_tie is None:
                first_tie = (path, frame, mover, shape)
            if rc != "0":
                broken.append(f"rc {rc}: {line.strip()}")
                continue
            old_ids = [int(x) for x in old.split(",")] if old else []
            if sorted(old_ids) != sorted(new_ids):
                broken.append(f"the two orders hold different ids: {line.strip()}")
            by_id = dict(zip(sorted(new_ids, key=lambda i: (i == -1, i)), scores))
            vals = [by_id[i] for i in new_ids]
            if any(vals[i] < vals[i + 1] for i in range(len(vals) - 1)):
                broken.append(f"new order is not descending: {line.strip()}")
            if shape == "list" and new_ids and new_ids[-1] != -1:
                broken.append(f"the -1 slot is not last: {line.strip()}")
            # equal scores keep the listed (ascending id) order
            for i in range(len(vals) - 1):
                if vals[i] == vals[i + 1] and new_ids[i] != -1 and new_ids[i + 1] != -1 and new_ids[i] > new_ids[i + 1]:
                    broken.append(f"equal scores out of listed order: {line.strip()}")
                    break
    total = calls["all"] + calls["list"]
    if malformed:
        print(f"MALFORMED {malformed}")
        return 1
    for shape in ("all", "list"):
        print(f"shape {shape}: {calls[shape]} calls, {ties[shape]} with equal scores, {differ[shape]} where the old rule and the new rule order differently")
    if first_tie:
        print("first call with ties: %s frame %s mover %s shape %s" % first_tie)
    for b in broken[:10]:
        print("BROKEN", b)
    print(f"summarize_sortweapons_probe: {'FAIL' if broken or not total else 'OK'} ({total} calls, {len(broken)} broken)")
    return 1 if broken or not total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
