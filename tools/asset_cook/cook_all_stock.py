#!/usr/bin/env python3
"""Stock viewer meshdumps -> one content-pipeline package (no parallel Track G cook)."""
import argparse
import json
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline.cook import cook
from tools.asset_cook.asset_cook_classify import classify_appearance, can_emit_override


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--meshdumps", type=Path, required=True)
    ap.add_argument("--deploy-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--overwrite", action="store_true")
    args = ap.parse_args()
    assets = []
    for source in sorted(args.meshdumps.glob("*.json")):
        dump = json.loads(source.read_text(encoding="utf-8"))
        if "submeshes" not in dump:
            continue
        # Animated appearances remain on the stock runtime compatibility frontend.
        aid = Path(dump["tgl"]).stem.lower()
        if not can_emit_override(classify_appearance(aid, args.deploy_root / "data/tgl")[0]):
            continue
        assets.append({"frontend": "stock_meshdump", "source": str(source)})
    spec = {"mod_id": "stock-content", "assets": assets,
            "texture_roots": [str(args.deploy_root / "data/tgl" / str(t)) for t in (128,256,512,1024)]}
    print(cook(spec, args.out, overwrite=args.overwrite))


if __name__ == "__main__":
    main()
