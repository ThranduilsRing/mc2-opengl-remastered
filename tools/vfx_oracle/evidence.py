"""Extract diagnostic engine evidence without inventing missing ownership or coverage."""
from collections import Counter
import hashlib
import re


def extract(log: str) -> dict:
    births = [line for line in log.splitlines() if line.startswith("[VFX_BIRTH v1] ")]
    draws = [line for line in log.splitlines() if line.startswith("[VFX_DRAW_RNG v1] ")]
    fields = [dict(re.findall(r"(\w+)=([^ ]+)", line)) for line in draws]
    for row, line in zip(fields, draws):
        name = re.search(r" spec=(.*?) time=", line)
        if name:
            row["spec"] = name[1]
    groups = Counter(tuple(row.get(key) for key in ("spec", "time", "age", "seed", "stream"))
                     for row in fields)
    frozen = [count for count in groups.values() if count >= 2]
    owned = bool(births) and all(" stream=56465852 " in line for line in births)
    return {"birth_count": len(births), "birth_records": births,
            "birth_transcript": hashlib.sha256("\n".join(births).encode()).hexdigest(),
            "rng": {"owner": None, "injected_stream_seen": owned},
            "draw_probe": {"scope": "CardCloud", "samples": len(fields),
                           "unchanged": bool(fields) and all(row.get("unchanged") == "1" for row in fields),
                           "frozen_repeats": max(frozen, default=0)},
            "limitations": ["RenderWorld reset/save-load ownership is not instrumented",
                            "CardCloud probe does not cover other classes, flush or culling",
                            "birth records do not establish visible weapon-family coverage"]}
