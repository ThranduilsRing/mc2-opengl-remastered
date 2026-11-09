"""MatSynth ingest: read metadata first, apply the licence gate, only then decode the images of passing rows.
Local parquet shards only (the caller downloads them to scratch). Needs pyarrow + Pillow."""
from __future__ import annotations
import io
import json
from pathlib import Path
import numpy as np

from . import licence_gate as lg

MAPS = ("basecolor", "roughness", "metallic")


def _img(cell, edge, gray):
    from PIL import Image
    im = Image.open(io.BytesIO(cell["bytes"]))
    im = im.convert("L" if gray else "RGB").resize((edge, edge), Image.BOX)
    return np.asarray(im, np.float32) / 255.0


def load(shards, categories, per_category=1, edge=32):
    """Returns ({key: dict(basecolor [e,e,3] sRGB 0..1, roughness [e,e], metallic [e,e], row=metadata row)}, refused)."""
    import pyarrow.parquet as pq
    picked, refused = {}, []
    for shard in shards:
        pf = pq.ParquetFile(shard)
        rows = pf.read(columns=["name", "category", "metadata"]).to_pylist()
        refused += lg.refused(rows)
        for r, _ in lg.select(rows, categories):
            cat = lg.CATEGORIES[r["category"]]
            if sum(1 for k in picked if k.startswith(cat + ":")) >= per_category:
                continue
            idx = [i for i, x in enumerate(rows) if x["name"] == r["name"]][0]
            t = pf.read(columns=list(MAPS)).slice(idx, 1).to_pylist()[0]
            if any(t[m] is None for m in MAPS):
                continue
            picked[f"{cat}:{r['name']}"] = dict(
                basecolor=_img(t["basecolor"], edge, False), roughness=_img(t["roughness"], edge, True),
                metallic=_img(t["metallic"], edge, True), row=r)
    return picked, refused


def write_provenance(path, picked):
    sets = {k.split(":", 1)[1]: lg.provenance_row(v["row"]) for k, v in picked.items()}
    doc = {"_note": "neural-n2 de-risk sources: MatSynth rows passing the CC0 licence gate only.", "sets": sets}
    Path(path).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
