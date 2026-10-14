"""Extract diagnostic engine evidence without inventing missing ownership or coverage."""
from collections import Counter
import hashlib
import math
import re


PHASES = ("draw", "flush", "cull")


def uint(value, bits):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+", value):
        raise ValueError("missing unsigned RNG observation")
    number = int(value)
    if number >= 2**bits:
        raise ValueError("RNG observation out of range")
    return number


def unchanged(row):
    try:
        for field, bits in (("state", 32), ("draws", 64), ("legacy", 64)):
            if uint(row.get(field + "_before"), bits) != uint(row.get(field + "_after"), bits):
                return False
        if row.get("scope") == "all" or "legacy_state_before" in row or "legacy_state_after" in row:
            before, after = row.get("legacy_state_before", ""), row.get("legacy_state_after", "")
            if not re.fullmatch(r"[0-9a-f]{64}", before) or before != after:
                return False
        return row.get("unchanged", "1") == "1"
    except ValueError:
        return False


def draw_probe(log):
    rows = []
    for line in log.splitlines():
        for phase in PHASES:
            if line.startswith(f"[VFX_{phase.upper()}_RNG v1] "):
                row = dict(re.findall(r"(\w+)=([^ ]+)", line))
                name = re.search(r" spec=(.*?) time=", line)
                if name:
                    row["spec"] = name[1]
                rows.append({**row, "phase": phase})
    groups = {}
    global_rows = [row for row in rows if row.get("scope") == "all"]
    valid = bool(rows) and all(unchanged(row) for row in rows)
    for row in global_rows:
        try:
            if row.get("owner") != "RenderWorld" or row.get("stream") != "56465852" or not row.get("spec"):
                raise ValueError("unowned or unidentified probe")
            time, age, seed = (float(row[key]) for key in ("time", "age", "seed"))
            if not all(math.isfinite(value) for value in (time, age, seed)) or time < 0 or age < 0 or not 0 <= seed <= 1:
                raise ValueError("invalid frozen effect identity")
            rng = {"seed": uint(row.get("initial"), 32),
                   "state": uint(row.get("state_before"), 32),
                   "draw_count": uint(row.get("draws_before"), 64)}
            legacy_count = uint(row.get("legacy_before"), 64)
            if not unchanged(row):
                raise ValueError("RNG changed or incomplete snapshot")
            key = (row["spec"], time, age, seed, *rng.values(), legacy_count, row["legacy_state_before"])
            group = groups.setdefault(key, {"spec": row["spec"], "time": time, "age": age,
                                            "effect_seed": seed, "rng": rng,
                                            "phase_samples": dict.fromkeys(PHASES, 0)})
            group["phase_samples"][row["phase"]] += 1
        except (KeyError, ValueError, OverflowError):
            valid = False
    states = [{**group, "frozen_repeats": min(group["phase_samples"].values())} for group in groups.values()]
    repeats = max((state["frozen_repeats"] for state in states), default=0)
    if not global_rows:
        counts = Counter(tuple(row.get(key) for key in ("spec", "time", "age", "seed", "stream"))
                         for row in rows if row["phase"] == "draw")
        repeats = max((count for count in counts.values() if count >= 2), default=0)
    scope = "CardCloud" if rows and all(row.get("class") == "CardCloud" for row in rows) else "partial"
    if any(state["frozen_repeats"] for state in states):
        scope = "all_draw_flush_cull"
    return {"scope": scope,
            "samples": len(rows), "unchanged": valid, "frozen_repeats": repeats,
            "phase_samples": {phase: sum(row["phase"] == phase for row in global_rows) for phase in PHASES},
            "frozen_states": states}


def extract(log: str) -> dict:
    births = [line for line in log.splitlines() if line.startswith("[VFX_BIRTH v1] ")]
    owned = bool(births) and all(" stream=56465852 " in line for line in births)
    return {"birth_count": len(births), "birth_records": births,
            "birth_transcript": hashlib.sha256("\n".join(births).encode()).hexdigest(),
            "rng": {"owner": None, "injected_stream_seen": owned},
            "draw_probe": draw_probe(log),
            "limitations": ["RenderWorld reset/save-load ownership is not instrumented",
                            "CardCloud-only observations cannot establish global draw/flush/cull coverage",
                            "birth records do not establish visible weapon-family coverage"]}
