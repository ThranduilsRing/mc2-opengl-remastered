"""Licence gate for third-party material data (MatSynth). Only metadata.license == "CC0" passes; everything else,
and the Deschaintre subsets (non-commercial), is refused. Pure metadata: no network."""
from __future__ import annotations
import json
from dataclasses import dataclass
from pathlib import Path

ALLOWED = ("CC0",)
EXCLUDED_SOURCE_PREFIXES = ("deschaintre",)
# MatSynth category index -> name (dataset card order)
CATEGORIES = ("ceramic", "concrete", "fabric", "ground", "leather", "marble", "metal", "misc", "plaster",
              "plastic", "stone", "terracotta", "wood")


@dataclass(frozen=True)
class Decision:
    ok: bool
    reason: str


def check(meta: dict) -> Decision:
    """meta is a MatSynth row's `metadata` dict."""
    lic = str(meta.get("license", "")).strip()
    src = str(meta.get("source", "")).strip().lower()
    if any(src.startswith(p) for p in EXCLUDED_SOURCE_PREFIXES):
        return Decision(False, f"excluded source {src!r}")
    if lic not in ALLOWED:
        return Decision(False, f"licence {lic!r} is not CC0")
    return Decision(True, "CC0")


def select(rows, categories=None):
    """rows: iterable of {name, category, metadata}. Returns [(row, Decision)] for passing rows only."""
    out = []
    for r in rows:
        d = check(r["metadata"])
        cat = CATEGORIES[r["category"]] if isinstance(r.get("category"), int) else r.get("category")
        if d.ok and (categories is None or cat in categories):
            out.append((r, d))
    return out


def refused(rows):
    return [(r["name"], check(r["metadata"]).reason) for r in rows if not check(r["metadata"]).ok]


def provenance_row(row: dict) -> dict:
    """One row in the tools/mc2texcook/cliff_rock_provenance.json 'sets' format."""
    m = row["metadata"]
    return {
        "asset": f"{m.get('name', row['name'])} ({row['name']})",
        "author": ", ".join(m.get("authors") or []) or str(m.get("source", "unknown")),
        "licence": "CC0 1.0",
        "licence_url": "https://creativecommons.org/publicdomain/zero/1.0/",
        "source_url": m.get("link") or f"https://huggingface.co/datasets/gvecchio/MatSynth (source: {m.get('source')})",
        "files": [f"MatSynth {mp}" for mp in m.get("maps", [])],
        "local": "G:/mc2-tmp/neural-n2/ms (scratch; not committed)",
    }


def load_metadata(path) -> list:
    return json.loads(Path(path).read_text(encoding="utf-8"))
